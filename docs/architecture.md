# dnuma architecture

dnuma turns several multiprocessor x86-64 servers connected by InfiniBand into
one asymmetric NUMA system. One machine, the **master**, runs the authoritative
Linux instance. Other machines, **slaves**, contribute CPU cores and physical
memory and are controlled by the master. Threads of one logical process run on
any machine and share one virtual address space.

This document describes what exists today, on which facts it rests, and where
the userspace implementation stands in for work that ultimately belongs in the
kernel. Statements about hardware and kernels come from inspecting the test
machines, not from assumption (see *Reference test environments*).

## Reference test environments

dnuma itself contains no machine names, addresses or device names; the
scripts and tests take theirs from a site file (`scripts/dnuma-env.example`).
What follows is only where it has been developed and tested, because
statements about hardware and kernels in these documents come from
inspecting real machines.

| | Master | Slave A (first half of development) | Slave B (since) |
|---|---|---|---|
| CPU | 4 x Xeon E5-4657L v2, 96 CPUs | same | same; the agent offers **one NUMA node** (24 CPUs, `--nodes`) because the machine has other work |
| NUMA | 4 nodes, ~126 GiB each | 4 nodes | 4 nodes, ~125 GiB each |
| Kernel | 7.3.0-rc1 (custom build) | 5.15 (Ubuntu) | 7.0 (Ubuntu) |
| InfiniBand HCA | Connect-IB (mlx5), 1 port, FDR10 40 Gb/s | same | same |
| HCA locality | physical NUMA node 1 | node 1 | node 0 (not the offered node) |
| RDMA limits | max_qp_rd_atom 16, no IB atomics, ODP supported | same | same |
| userfaultfd | missing, wp, minor, move, rwp, poison, `/dev/userfaultfd` | missing, wp, minor (no move/rwp/poison/dev node) | missing, wp, minor, move, poison, `/dev/userfaultfd` |
| Toolchain | gcc 11.4, cmake 3.22, rdma-core 39 | same | gcc 15, rdma-core 61 |

Slave A failed (hardware) during development and was replaced by slave B;
`docs/testing.md` says which results come from which. The build directory
was visible on both machines through a shared file system, which is a
convenience of `scripts/cluster.sh` and not something dnuma relies on:
process export in particular needs nothing on the slave but the agent.

Two facts about kernels shape the design: a slave may have no userfaultfd
feature newer than 5.15's, and no kernel lets one process remove a mapping
from another (see `memory.md`, "What the kernel permits").

## Components

```
                      master                                            slave        
  ┌───────────────────────────────────────────┐        ┌─────────────────────────────┐
  │ dnumactl ──unix──┐                        │        │                             │
  │                  ▼                        │  TCP   │                             │
  │   dnuma-master ──────────── management ───┼────────┼──► dnuma-agent              │
  │   ├ registry + lifecycle (slaves)         │ (auth) │    ├ lease / self-fencing   │
  │   ├ topology (logical NUMA)               │        │    │                        │
  │   ├ page directory + coherence engine     │  RDMA  │    │                        │
  │   ├ process / execution manager           │  RC QP │    │                        │
  │   └ NodeCore (machine 0) ◄── ops + pages ─┼────────┼──► └ NodeCore (machine N)   │
  │        ▲ uffd, pagemap, pidfd             │        │         ▲ uffd, pagemap     │
  │        │ unix                             │        │         │ socketpair        │
  │   home image (the user's process)         │        │    shadow image             │
  │   = program + libdnuma                    │        │    = same program + libdnuma│
  └───────────────────────────────────────────┘        └─────────────────────────────┘
```

| Component | Source | Role |
|---|---|---|
| `dnuma-master` | `src/master/` | The single authority. Slave registry and lifecycle, logical topology, page directory and coherence decisions, logical processes and execution contexts, draining, fencing, control socket. Also hosts the NodeCore for the master's own memory and images. |
| `dnuma-agent` | `src/agent/` | Runs on each slave. Authenticates the master, reports hardware, hosts a NodeCore, renews its lease, and fences itself whenever its attachment ends. Makes no global decisions. |
| NodeCore | `src/node/` | The per-machine memory manager and execution host, shared by master and agent: owns each local image's userfaultfd, resolves faults, holds parked pages in the registered pool, applies grants and revocations, pulls pages over RDMA, creates images. |
| `libdnuma` | `src/runtime/`, `include/dnuma/dnuma.h` | Linked into distributed programs. Reserves the distributed range, hands its userfaultfd to the node daemon, hosts execution contexts, forwards system calls. |
| `dnuma-run` | `src/cli/dnuma_run.cc` | Launcher: becomes the program (exec), which registers as the home image. |
| `dnumactl` | `src/cli/dnumactl.cc` | Management CLI over the master's Unix control socket. |
| transport | `src/transport/` | Wire format, authenticated stream channels, RDMA links. |

Module boundaries follow the brief: transport (`src/transport`), memory
coherence (`src/master/coherence.*` decides, `src/node` enforces), process
management (`src/master/master_proc.cc`, `src/runtime`), lifecycle
(`src/master/lifecycle.*`, `master_slaves.cc`, `src/agent`).

Every daemon is a single-threaded epoll reactor: sockets, RDMA completion
channels, CM events, userfaultfds, pidfds, signals and timers are all event
sources. Nothing polls.

## Identities

| Identity | Meaning | Lifetime |
|---|---|---|
| **slave UUID** | Stable identity of an agent installation, stored in its state directory. Distinct from hostname and from the administrative name used with `dnumactl`. | Survives agent restarts and reboots. |
| **MachineId** | Small integer bound to a UUID (0 = master). | Master's lifetime. |
| **incarnation** | Fresh 64-bit value for every attachment of a slave (start-time seconds << 20 + counter, so values do not repeat across master restarts). | One attachment. |
| **master instance** | Random 64-bit value chosen at master start; the master's side of every ticket and link. | One master run. |
| **NodeId** | Internal identity of one NUMA domain: (machine UUID, kernel node index). | Master's lifetime; never reassigned. |
| **logical NUMA number** | User-visible number of a NodeId. Assigned in first-attach order and sticky: a slave that reattaches gets its old numbers; numbers of an absent slave are not given to anyone else. | Master's lifetime. |
| **ProcId** (DPID) | Logical process. | Until the home image exits or the process is terminated. |
| **CtxId** | Logical thread (execution context). | Until joined. |

Every message that changes state carries the incarnation it is meant for;
anything else is dropped (`stale_messages_dropped`). Per-page operations
additionally carry the page generation.

## Logical processes versus native Linux processes

Linux cannot schedule a task of the master's kernel onto a CPU of the slave. dnuma
does not pretend otherwise. A *logical* process is a set of *native* processes:

* the **home image** on the master: the very process the user started. It owns
  the Linux-visible identity - PID, credentials, file descriptors, working
  directory, signal dispositions, futex queues, exit status;
* at most one **shadow image** per slave: the same executable (verified by
  SHA-256), started by that slave's agent the first time an execution context
  is placed there. It never runs `main()`. Natively it is an unrelated process
  with its own PID; logically its threads are threads of the home process.

What is authoritative where:

| State | Authority | How the other side sees it |
|---|---|---|
| Process identity, thread identities, placement | master (`Process`, `ExecCtx`) | told to nodes in EXEC_* messages |
| Virtual address layout of distributed memory | master (regions in the directory) | fixed range reserved identically in every image |
| Page ownership and contents | master directory decides; exactly one machine holds the authoritative copy | grants/revocations (see `memory.md`) |
| Registers, stack of an execution context | the machine running it; when suspended, entirely on its stack in distributed memory | moves with the pages (see `execution.md`) |
| File descriptors, credentials, cwd, futex queues | home image's kernel | reached by forwarding system calls (see `execution.md`) |
| Static data, malloc heap, TLS | **each image separately** | not shared - a documented limitation |
| Local page tables, physical placement, CPU scheduling | each machine's own kernel | reported, not controlled |

The last-but-one row is the main thing between this and transparent execution
of ordinary *multi-threaded* programs: only memory obtained from
`dnuma_alloc()` (and context stacks) is distributed. See `execution.md` for
the precise list of what transparent pthread support still needs.

### The other kind: exported processes

There is a second, independent way to use slave CPUs, for programs that know
nothing of dnuma. An ordinary single-threaded process on the master can be
**exported**: its user code then executes in a *body* on a slave, while the
process itself stays on the master and executes every system call the body
makes. Here the whole address space travels (lazily, page by page) and
nothing is shared, so none of the limitations above apply - but one process
runs on one machine at a time. `docs/export.md` describes it; the components
are `src/master/export*.cc` (home side), `src/agent/body.cc` (slave side)
and `src/export/` (system-call descriptors, messages).

## Channels

1. **Management channel** (TCP, master dials agent): mutual authentication,
   version negotiation, attach, NUMA identity assignment, lease renewal,
   resource reports, drain/fence/detach, peer-link setup, fault injection.
2. **Operations channel** (messages over RDMA SEND/RECV on the RC queue pair,
   or an in-process loopback for the master's own node): page protocol, image
   and execution control, forwarded system calls, application calls.
3. **Bulk data** (RDMA READ on the same queue pair): page contents, pulled by
   the destination directly from the source machine - including slave to
   slave, where the master only coordinates.
4. **Runtime channel** (Unix socket image <-> node daemon, with descriptor
   passing): registration, unmap requests, execution-context control.
5. **Control socket** (Unix): `dnumactl`.

Details in `protocol.md`.

## Security model

* Master and agents share a cluster key file. The management channel is
  mutually authenticated by challenge/response and every later frame carries
  a MAC with a sequence number. It is **not encrypted**.
* An RDMA connection request carries a MACed ticket naming both incarnations;
  an agent accepts links only from the master instance it is attached to and
  from peers the master announced.
* Each RDMA link has its own protection domain. A node registers in it (a) its
  page pool, remotely *readable* only, and (b) a 64 KiB inbox used solely for
  the attach-time self-test, the only remotely *writable* memory. No peer can
  write to pool pages at all: page data is always pulled. Destroying the link
  destroys its queue pair and registrations, which is how RDMA access is
  revoked (verified by `tests/failure/f3_partition.sh`).
* Known limits: every authenticated peer of an incarnation can read every pool
  slot of a machine (peers are privileged daemons of one trust domain);
  images of different users on one slave are separated only by ordinary Linux
  permissions; there is no confidentiality on any channel.

## What is implemented, tested, or only designed

| Area | Status |
|---|---|
| Control plane, attach/detach, identities, leases | implemented; tested on a master and one physical slave |
| Offering a subset of a slave's NUMA nodes (`dnuma-agent --nodes`) | implemented; tested (the whole suite runs against a one-node agent) |
| RDMA links, per-link PDs, verified READ/WRITE, credit flow control | implemented; tested |
| Page directory, MSI-style coherence, invalidation, migration, populate, abort | implemented; unit-tested with simulated multi-slave interleavings; tested on hardware |
| Remote execution on real CPUs, placement verification | implemented; tested (all 8 NUMA domains) |
| Cooperative thread migration (at migration points) | implemented; tested (drain) |
| Forwarded system calls, cross-machine futex and mutex | implemented; tested (`getpid`, `write`, futex) |
| Drain (with capacity check, timeout, cancel) | implemented; tested |
| Hard disconnect, failure detection, fencing, lease proof | implemented; tested |
| Two slaves, slave-to-slave transfers | implemented; tested with a second agent co-located on the master's host - **not** with a third machine |
| Pager-side unmap (`process_madvise`) | implemented and probed at run time; needs the kernel patch in `kernel/untested/`, which is compile-checked but **not booted** |
| Process export: unmodified single-threaded programs moved to a slave while running, system calls and I/O executed on the master, automatic balancer | implemented; tested (`docs/export.md`) |
| Slave CPUs and exported work visible in the master's `/proc` (top, ps, `/proc/cpuinfo`) | implemented; needs the kernel view series (`kernel/patches/`) on the master. Kernel side and the master's client tested in a virtual machine; end to end see `docs/testing.md` |
| Transparent pthreads, automatic syscall interception, distributed heap/TLS/static data | **not implemented** (design in `execution.md`) |
| Preemptive (asynchronous) thread migration, XSAVE transfer | **not implemented** (design in `execution.md`) |
| Automatic page reclaim/eviction policy, scheduler policies beyond explicit and least-loaded | **not implemented** |
| LOST page state after partial failure | state exists; never entered, because dependents of a failed slave are killed (the conservative rule in the brief) |
| Kernel-visible NUMA nodes, kernel-scheduled remote tasks, slave CPUs in affinity masks | **not implemented**; see `kernel/README.md` |
| Master restart / persistence / high availability | **not implemented**: one master, in-memory state |
