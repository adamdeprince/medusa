# Slave lifecycle, draining, fencing and failure

## State machine

Implemented as a pure transition table in `src/master/lifecycle.cc`; the
master changes a slave's state only through it, so an undocumented transition
cannot happen. Transitions are serialized per slave because the master is a
single-threaded reactor; concurrent administrative requests are simply handled
one after another against the current state.

```
                      connect                       connect-ok
   DISCONNECTED ──────────────────► CONNECTING ──────────────────► ACTIVE
        ▲   ▲                        │   │                          │  │
        │   │        connect-fail    │   │ failure                  │  │
        │   └────────────────────────┘   │            drain         │  │
        │                                │      ┌───────────────────┘  │
        │        drain-done              │      ▼                      │
        ├─────────────────────────── DRAINING ──── drain-cancel ──► ACTIVE
        │                                │      │                      │
        │                                │      │ failure              │ failure
        │                                ▼      ▼                      ▼
        │                              FAILED ◄────────────────────────┘
        │                                │ fence-begin
        │                                ▼
        │   fence-confirmed           FENCING ◄──── hard-disconnect (from ACTIVE,
        ├──────────────────────────────  │                DRAINING or CONNECTING)
        │                                │ fence-unconfirmed
        │   lease-expired                ▼
        └──────────────────────── FENCE_INCOMPLETE ── connect ──► CONNECTING
```

| State | Meaning | New placement | Serves existing pages/contexts |
|---|---|---|---|
| `DISCONNECTED` | Not attached; any earlier incarnation is proven stopped. | no | no |
| `CONNECTING` | Attach handshake in progress. | no | no |
| `ACTIVE` | Attached, leased, RDMA link verified. | yes | yes |
| `DRAINING` | Graceful evacuation in progress. | no | yes |
| `FAILED` | Unexpected failure detected (explicit, transient: fencing starts immediately). | no | no |
| `FENCING` | Master-side isolation done; waiting (bounded) for the agent to confirm. | no | no |
| `FENCE_INCOMPLETE` | Master side is isolated but the remote machine is **not proven stopped**. Its resources are not reused. | no | no |

Idempotence: `connect` on an ACTIVE slave reports "already attached" (exit 0)
and changes nothing; on a CONNECTING slave reports the stage; on a DRAINING or
FENCING slave is refused. `disconnect` on a DISCONNECTED slave is a no-op.
A reconnect that fails while an earlier incarnation is unproven returns to
`FENCE_INCOMPLETE`, never to `DISCONNECTED`.

`dnumactl slave status <name>` prints the recent transition history.

## Attach (`dnumactl slave connect <name> [--address A] [--port P]`)

1. Resolve and connect to the agent's management port.
2. Mutual authentication (HMAC challenge/response with the cluster key);
   protocol version negotiation; MAC on every later frame.
3. The agent's UUID determines the MachineId. An agent already known under
   another name is refused.
4. `NODE_ATTACH` with a fresh incarnation. The agent destroys everything of
   any earlier incarnation and reports how much; it answers with its hardware
   report, RDMA endpoint, pool size and offered capacity.
5. NodeIds and logical NUMA numbers are assigned (or re-activated) and sent
   to the agent.
6. RDMA connection with a MACed ticket naming both incarnations; per-link
   protection domain; pool and inbox registered; `LINK_HELLO` exchanged.
7. Self-test in both directions: RDMA WRITE of 16 KiB of random data into
   the peer's inbox, RDMA READ back, byte comparison, latency recorded.
8. Links to every other serving slave (`PEER_EXPECT` / `PEER_CONNECT`).
9. First lease renewal received.
10. `ACTIVE`: eligible for allocation and execution.

Any failure or the attach timeout (20 s) tears down what was built and
reports the stage it failed in. Nothing requires restarting the master or any
running process.

## Leases and fencing

A network partition is not proof that a machine has stopped. dnuma therefore
separates three questions: has the master stopped trusting the slave
(immediate), can the slave still touch anything (revoked by destroying the
RDMA link), and has the slave stopped executing (needs proof).

* The **agent** renews its lease every `--renew-ms` (1 s) by sending a
  heartbeat; the lease it holds runs for `--lease-ms` (5 s) *from the moment
  it sent the renewal that was granted*. If the lease runs out, the agent
  fences itself: it kills every image with SIGKILL and waits for them, drops
  every RDMA link and forgets the incarnation. It does the same when the
  management connection or the RDMA link to the master is lost, when told to
  (`NODE_FENCE`, `NODE_DETACH`), and before accepting any new attach.
* Images set `PR_SET_PDEATHSIG(SIGKILL)`: if the agent process itself dies,
  the kernel kills its images.
* The **master** grants a lease only to serving slaves and records, each time
  it answers a renewal, the time after which that lease has certainly expired
  (`answer time + lease + 25% margin`). Because the agent counts from its send
  time, which precedes the master's answer, the master's bound is always the
  later one. The margin covers clock-rate difference and scheduling delay.

Fencing a slave (`begin_fence`), used identically for hard disconnect and
for every detected failure:

1. The state is already `FENCING`: no new placement, grants or messages.
2. Every dependent process is terminated (below).
3. Transfers heading to the slave are aborted; sources keep their pages.
4. The RDMA link is destroyed - queue pair, protection domain and all
   registrations - and other slaves are told to destroy their links to it.
5. `NODE_FENCE` is sent if the management connection is up. The master waits
   at most 3 s for the acknowledgement.

Outcome and the proof recorded (`dnumactl slave status`):

| Proof | When | State |
|---|---|---|
| `agent-confirmed` | The agent acknowledged destroying the incarnation (with counts). | `DISCONNECTED` |
| `clean-detach` | Graceful drain completed. | `DISCONNECTED` |
| *(none yet)* | No acknowledgement: unreachable, silent or dead. | `FENCE_INCOMPLETE` |
| `lease-expired` | The recorded lease bound has passed. | `DISCONNECTED` |
| `reattach-handshake` | The agent reattached and reported destroying its stale state. | (attach continues) |

Assumptions behind `lease-expired`, stated plainly: the agent's clock does not
run more than 25% slow relative to the master's, and either the agent process
is running (it self-fences) or it is dead (its images die with it). **Not
covered:** an agent process that is stopped or hung while its images keep
running - they could continue computing on memory they already hold (they
cannot fetch pages, forward system calls or reach any other machine, since the
agent mediates all of that and the master has destroyed the link). Closing
this gap needs a lease check inside each image or in the kernel; it is listed
as open work.

## Dependent processes

A process **depends** on a slave if any of its pages has a copy there - even
a read replica that also exists elsewhere - or any of its execution contexts
is there (the conservative rule from the brief). An idle shadow image with no
pages and no contexts is not a dependency; it is simply forgotten.
Memory objects are per process today, so "processes sharing affected memory
objects" adds nothing yet; `dependents_of()` is the single place to extend.

Terminating a process means: SIGKILL the home image through its pidfd; tell
every machine to forget the process, which kills its image there and frees its
pages; drop its directory entries, queued faults and transactions; record it
in the forced-termination log. Processes that do not depend on the slave are
not touched.

## Hard disconnect (`--hard`)

* `--dry-run` prints the processes that would be terminated and the memory
  that would be discarded, and changes nothing.
* Without `--yes`, `dnumactl` shows the dry run and asks for the slave's name
  on a terminal, and refuses outright when not interactive. The master itself
  also refuses a hard disconnect without confirmation.
* The real operation is the fencing procedure above and reports the processes
  it killed and the fencing verdict. If fencing is incomplete it says so and
  exits with status 3 rather than claiming success.

It never waits indefinitely for the slave: the only wait is the bounded
acknowledgement window.

## Graceful drain (`--drain [--timeout D]`)

Implemented in `Master::drain_start/drain_step/drain_pages`. Phases:

1. `DRAINING`: no new allocations, contexts or migrations target the slave.
   The cost is checked first: if the pages that exist only on the slave do not
   fit in the remaining machines' free capacity, the drain fails immediately.
2. **Execution.** Every context running on the slave is asked to migrate
   (to the master). A context moves when it reaches a migration point; one
   that exits instead is equally fine. Every exported process
   (`docs/export.md`) on the slave is recalled, which is always possible: its
   body stops, its modified pages return to the process on the master, and
   it continues there. The drain proceeds when neither is left.
3. **Pages.** Each page with a copy on the slave is handled through the
   coherence engine: a read replica is dropped; an owned page that has a
   replica elsewhere has its ownership handed to that replica (no data moves);
   any other owned page is transferred to a machine with capacity - the master
   first, then other ACTIVE slaves - by a direct RDMA pull by the destination.
   Up to 64 transfers run concurrently. The directory is rescanned until the
   slave holds nothing; a rescan that makes no progress fails the drain.
   Allocations whose home node was on the slave are re-homed.
4. **Images.** Idle shadow images on the slave are destroyed.
5. **Verify and detach.** The master checks that no page, context, image,
   dependent process or transaction involves the slave, sends `NODE_DETACH`,
   and on acknowledgement destroys the link: `DISCONNECTED`, proof
   `clean-detach`.

While it runs, `dnumactl` prints a progress block every second, computed from
the directory at that moment (contexts on the slave, pages remaining, pages
existing only there, bytes to evacuate, transactions in flight).

Failure, timeout or `dnumactl slave cancel-drain <name>`: the drain stops, the
precise blocker is printed (the contexts that have not reached a migration
point, by id and process; or the pages remaining), the slave returns to
`ACTIVE`, no process is terminated and no page is discarded. Work already done
(contexts and pages already moved) is kept. A failed or timed-out drain is
**never** converted into a hard disconnect; the transition table has no such
edge.

## Unexpected failure

Detected by: management connection closed or reset (agent death, machine
shutdown); RDMA disconnect or completion error; no lease renewal for a full
lease period (silent partition, hung agent). Any of these marks the slave
`FAILED` and starts fencing immediately. Processes that were exported to the
slave are killed with the other dependents: the memory their bodies had
modified cannot be recovered. While a slave is attached its CPUs may be
visible in the master's `/proc` (the kernel view); they disappear from it the
moment the slave leaves `ACTIVE`/`DRAINING`. The master's reactor never blocks on
the failed slave, so other slaves and unrelated processes are unaffected.

## Reconnection

Every attach uses a new incarnation. The agent destroys stale state before
answering, old RDMA registrations died with the old link, and tickets name
the exact incarnations, so a stale incarnation can neither be mistaken for the
new one nor reach its memory. NodeIds and logical NUMA numbers are unchanged.
