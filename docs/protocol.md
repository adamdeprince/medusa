# Wire protocol

Protocol version 1 (`kProtoVersion`). Definitions: `src/transport/wire.h`
(framing, message types), `src/transport/messages.h` (bodies),
`src/transport/auth.h` (authentication), `src/transport/rdma.{h,cc}`
(RDMA framing and tickets).

## Frame

Every message on every channel is one frame:

```
offset  size  field
0       4     magic        0x414d4e44 ("DNMA")
4       2     version      1
6       2     type         MsgType
8       4     payload_len  <= 64 MiB
12      4     nfds         descriptors attached (Unix sockets only, <= 8)
16      8     request_id   correlates a reply with its request (0 = none)
24      8     incarnation  slave incarnation the message is meant for / from
32      n     payload
[32+n   16    MAC          authenticated stream channels only]
```

All integers are little-endian. A receiver validates magic, version, length
and descriptor count before buffering the payload; a frame that fails is
treated as corruption and the channel is closed. Bodies are decoded with a
bounds-checked reader: any short field, any element count that could not fit
in the remaining bytes, and any trailing byte make the whole message invalid.
Each body lists its fields once (`io()`), and the same list drives encoding
and decoding. Tests: `tests/unit/test_wire.cc`.

The 64 MiB payload bound and the 8-descriptor bound are protocol constants
that limit what a peer can make a daemon buffer; they are not sizes of any
fixed buffer.

## Reliability, ordering, retries, timeouts

All channels are reliable and ordered (TCP, Unix stream, RDMA RC), so there is
no message-level retransmission. What the protocol adds:

* **Acknowledgements.** Every operation that changes state is answered by a
  specific message (`NODE_ATTACH` -> `NODE_ATTACH_ACK`, `PAGE_REVOKE` ->
  `PAGE_REVOKED`, `PAGE_GRANT` -> `PAGE_TRANSFER_COMPLETE`, `EXEC_CREATE` ->
  `EXEC_STARTED`, ...), carrying a status.
* **Staleness.** A message whose incarnation is not the current one is dropped
  and counted. Page messages also carry the page generation; a mismatch is
  ignored.
* **Timeouts** are per operation: attach 20 s overall (TCP connect 5 s, RDMA
  connect 8 s); fence acknowledgement 3 s; detach acknowledgement 5 s; image
  creation 30 s (20 s on the node); cooperative unmap 10 s (then the image is
  killed); lease 5 s. A timeout is handled as the failure it indicates - there
  are no blind retries of state-changing operations.
* **Retries** exist at the transport level only: the RC queue pair retries 7
  times (transport) and indefinitely on receiver-not-ready, and the message
  layer's credits make receiver-not-ready impossible in normal operation.

## Management channel (TCP, master -> agent:7710)

Handshake (unauthenticated frames that carry their own proofs):

```
master -> agent  HELLO      {ver_min, ver_max, master_instance, nonce_m[32]}
agent  -> master HELLO_ACK  {status, version, uuid, hostname, nonce_a[32],
                             HMAC(key, "dnuma-agent-proof" | nonce_m | nonce_a | uuid)}
master -> agent  AUTH       {HMAC(key, "dnuma-master-proof" | nonce_a | nonce_m | instance)}
```

Each field in a proof is length-prefixed. After `AUTH` both directions append
`HMAC(session_key, seq | header | payload)[0..16]` to every frame, with
`session_key = HMAC(key, "dnuma-session-key" | nonce_m | nonce_a)` and a
per-direction sequence number, so frames cannot be forged, replayed or
reordered. The cluster key never crosses the wire. The channel is not
encrypted.

| Message | Direction | Purpose |
|---|---|---|
| `NODE_ATTACH` / `_ACK` | M->A / A->M | New incarnation; agent destroys stale state and returns `RESOURCE_REPORT` data (hardware, RDMA endpoint, pool, capacity). The request also says how often the master wants `VIEW_SAMPLE` (0 = never) |
| `NODE_ASSIGN` / `_ACK` | M->A | NodeId and logical number of each physical NUMA node |
| `HEARTBEAT` / `_ACK` | A->M / M->A | Lease renewal request / grant (`lease_ms`, 0 = refused) |
| `RESOURCE_REPORT` | A->M | Every 2 s: free memory per node, resident pages per node, pool use, images, contexts, metrics snapshot |
| `VIEW_SAMPLE` | A->M | Only if requested at attach (every 500 ms): the agent's clock-tick rate; for each CPU it offers, the ten counters of that CPU's `/proc/stat` line; for each body, its export id, state character, user and system time. Feeds the master's kernel view (`docs/export.md`) |
| `NODE_DRAIN` | M->A | Informational: a drain has started |
| `NODE_FENCE` / `_ACK` | M->A | Destroy the incarnation; ack carries image and context counts |
| `NODE_DETACH` / `_ACK` | M->A | End of a completed drain |
| `PEER_EXPECT`, `PEER_CONNECT`, `PEER_STATUS`, `PEER_FENCE` | M<->A | Slave-to-slave link setup and teardown |
| `FAULT_INJECT` / `_ACK` | M->A | Test hooks: `pull-delay <ms>`, `drop-rdma`, `partition <ms>`, `exit` |

## RDMA link

Connection management uses librdmacm (`RDMA_PS_TCP`, RC). The connect request
and reply each carry a 48-byte ticket in CM private data (InfiniBand leaves 56
bytes for it, a constraint of the CM REQ format):

```
magic(4) version(2) kind(2: 0 request, 1 reply) src(4) dst(4) src_inc(8) dst_inc(8)
HMAC(key, "dnuma-rdma-link" | preceding fields)[0..16]
```

The master's "incarnation" is its instance id. The acceptor checks that the
ticket is addressed to its current incarnation and comes from the master
instance it is attached to, or from a peer the master announced with exactly
that incarnation.

**Messages** are frames carried in SEND/RECV, split into fragments of at most
128 KiB (one registered message buffer), each with a 3-byte header: flags
(`more`, `credit-only`) and a 16-bit credit grant. Flow control is credit
based: a side sends a fragment only when the peer has a receive buffer
posted for it (256 per link); credits return piggybacked or in a credit-only
fragment once 32 accumulate, and one credit is always reserved for that,
which keeps the scheme deadlock-free. Messages of any size up to the payload
bound (64 MiB) are supported.

**Registered memory** per link (own protection domain): the page pool,
remote-read only; a 64 KiB inbox, remote read/write, used by the self-test.
`LINK_HELLO` exchanges base addresses, lengths and rkeys. A page reference in
the protocol is `(machine, incarnation, slot)`; the puller computes the remote
address from the pool base it learned for that link.

## Operations channel

Carried over the RDMA link (slaves) or an in-process queue (the master's own
node). `C` = coordinator (master), `N` = node.

Pages (one body type, `PageMsg`, for all of them: process, page, generation,
access, mode, source, flags, status, source machine, source incarnation, slot,
home node):

| Message | Direction | Meaning |
|---|---|---|
| `PAGE_REQUEST` | N->C | A local thread faulted; need read or write access |
| `PAGE_REVOKE` | C->N | Give up access: `export`, `downgrade+export`, `invalidate+export`, `invalidate` |
| `PAGE_REVOKED` | N->C | Done (verified); slot of the staged copy if exported |
| `PAGE_GRANT` | C->N | Access granted; data source is `zero`, `local` or `remote(machine, incarnation, slot)`. A non-zero status refuses the request (unallocated address) |
| `PAGE_TRANSFER_COMPLETE` | N->C | Grant applied (or failed with a status) |
| `PAGE_RELEASE` | C->N | Destination confirmed: free the staged copy |
| `PAGE_ABORT` | C->N | Transfer failed: staged copy becomes the page again |
| `MEMORY_ALLOCATE` / `_ACK` | C->N / N->C | Materialize zero pages parked on the node (populate) |
| `MEMORY_RELEASE` | C->N | Forget a process: kill its image, free its pages |

Images and execution (`ImageMsg`, `ExecMsg`):

| Message | Direction | Meaning |
|---|---|---|
| `IMAGE_CREATE` | C->N | Start a shadow image: executable path and SHA-256, argv, address range, canary, uid/gid |
| `IMAGE_READY` | N->C | Image registered its userfaultfd (or failed, with reason) |
| `IMAGE_EXITED` | N->C | Image process exited (status) |
| `IMAGE_DESTROY` | C->N | Kill an idle image (drain) |
| `EXEC_CREATE` | C->N | New context: entry address, argument, stack range, NodeId |
| `EXEC_START` | C->N | Release a created context |
| `EXEC_STARTED` | N->C | Running: host, native tid, CPU, physical node (or a failure status) |
| `EXEC_SUSPEND` | C->N | Suspend at the next migration point |
| `EXEC_SUSPENDED` | N->C | Suspended; carries the saved stack pointer |
| `EXEC_MIGRATE` | C->N | Resume a suspended context here (saved stack pointer, stack range, NodeId) |
| `EXEC_TERMINATE` | C->N | Cancel at the next migration point (implemented in the runtime; not yet used by the master) |
| `EXEC_EXITED` | N->C | Finished: result, flags |
| `SYSCALL_REQUEST` | N->C->home node | Execute a system call in the home image |
| `SYSCALL_COMPLETE` | home node->C->N | Result and errno |
| `APP_CALL` / `APP_REPLY` | N->C / C->N | libdnuma requests: alloc, free, thread create/join/migrate, topology, migrate pages, page info |
| `LINK_HELLO`, `LINK_VERIFY` / `_ACK` | both | Link setup and self-test |

A node stamps the process id and origin machine of everything an image sends,
so an image can only ever act on its own logical process.

Process export (`docs/export.md`; bodies in `src/export/xmsg.h`). Every
message carries the export id, which also selects the worker thread that
owns the process on each side:

| Message | Direction | Meaning |
|---|---|---|
| `X_BEGIN` | C->N | Create a body: registers, extended register state, the list of mappings, the CPU to run on and the number it has on the master, the master's clock readings |
| `X_READY` | N->C | The body is running (or could not be created, with the reason) |
| `X_PAGE_REQ` | N->C | The body touched memory it does not have: address and how many pages are wanted (16 to 256; the window grows with sequential access) |
| `X_PAGE` | C->N | Contents of those pages, read from the process at home; or a status if the address is not readable there |
| `X_SYSCALL` | N->C | The body made a system call: number, arguments, and the input data its pointer arguments refer to |
| `X_SYSRET` | C->N | Result of executing it at home, output data, and for calls that change the address space the resulting mappings |
| `X_STOP` | C->N | Stop the body and send its state home (signal, recall, drain) |
| `X_HOME` | N->C | The body has stopped: registers, why (requested, a call that must run natively, a fault, exit, error), the latest clock values it has seen |
| `X_PAGE_PUSH` | N->C | Pages the body modified, on the way home |
| `X_HOME_DONE` | N->C | Everything has been sent; the body is gone |
| `X_KILL` | C->N | Destroy the body (the process died or was killed at home) |
| `X_GONE` | N->C | The body died unexpectedly |
| `X_CLOCK_REQ` / `X_CLOCK` | N->C / C->N | Clock comparison: the slave's readings of the five system clocks, returned together with the master's |

All export traffic is ordinary messages; RDMA READ/WRITE is used only for the
page pool of `dnuma-run` programs.

## Runtime channel (Unix socket, image <-> node daemon)

`RT_HELLO` (role, argv, stack canary) -> `RT_WELCOME` (process id, address
range, canary, uid/gid; on the master also a `/dev/userfaultfd` descriptor)
-> `RT_READY` (userfaultfd descriptor attached, feature bits, whether
kernel-mode faults are handled). Then `RT_ZAP` / `RT_ZAP_DONE` (unmap a range),
the `EXEC_*`, `SYSCALL_*` and `APP_*` messages above. If this connection
closes, the daemon kills the image and the image kills itself.

## Control socket (Unix, `dnumactl` <-> master)

`CTL_REQUEST {args[]}` -> zero or more `CTL_OUTPUT {text}` -> `CTL_DONE
{exit code}`. Output is rendered by the master from live state; long
operations (connect, drain) stream progress.
