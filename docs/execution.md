# Remote execution

## Images

A distributed program is linked statically and non-PIE against `libdnuma`
(`dnuma_add_executable()` in CMake adds `-static -no-pie -Wl,--wrap=main`), so
its code and static data have identical addresses in every image; a function
pointer is valid on every machine. The agent refuses to start an image whose
executable differs from the master's copy (SHA-256). Today the executable must
be reachable at the same path on the slave (here: the shared NFS home);
shipping the binary over the protocol is not implemented.

`__wrap_main` runs before `main`:

* **Home image** (started by `dnuma-run`, which sets `DNUMA_SOCK` and execs
  the program): registers with the master, sets up the distributed range and
  its userfaultfd, starts the runtime service thread, then calls the real
  `main`. The process is an ordinary child of the user's shell.
* **Shadow image** (started by an agent): sets `PR_SET_PDEATHSIG`, registers
  over the socket it inherited, sets up the range, adopts the home image's
  stack-protector canary, drops privileges to the user's uid/gid, and becomes
  the service loop. `main` never runs there.

## Execution contexts

An execution context is a logical thread. Natively it is a **fiber** - a stack
in distributed memory plus saved registers - run by a **carrier**: an ordinary
pthread of the hosting image, pinned to the CPUs of the requested NUMA node.

`dnuma_thread_create(node, fn, arg)`:

1. The master resolves the logical node, refuses if its machine is not
   accepting work (placement is never silently changed), allocates a stack in
   the distributed range with that node as home, and ensures an image exists
   on the machine.
2. `EXEC_CREATE` + `EXEC_START` go to the machine's node, which adds the
   node's CPU list and forwards them to the image.
3. The carrier pins itself, reports `EXEC_STARTED` (native tid, CPU), builds
   the initial frame on the distributed stack - the first stores fault the
   stack page in on that machine - and switches to it.
4. On return the carrier reports `EXEC_EXITED` with the result;
   `dnuma_thread_join` delivers it together with where the context ran.

Verification of "really ran there" uses two independent sources: the function
calls `dnuma_where()` (hostname, `getcpu`) and stores the answer in shared
memory, and the hosting daemon reports the carrier's tid, CPU and physical
node (`t_exec`, all 8 NUMA domains).

### The three runtime rules

Paging a process's own stack from underneath it makes ordinary locking
dangerous. The runtime follows three rules (`src/runtime/runtime.cc`):

* **R1** The service thread never touches distributed memory. It carries out
  unmap requests for the pager; if it could block on a fault, the pager could
  wait on it forever.
* **R2** No runtime lock is held by a thread running on a distributed stack.
  Whenever a fiber calls into the runtime it first switches to its carrier's
  private stack (`on_carrier()`), the way a system call switches to a kernel
  stack; the call runs there and switches back.
* **R3** Everything sent on the runtime socket is serialized into private heap
  memory before the send lock is taken.

Application code is not covered by these rules: a program that holds a libc
lock (stdio, malloc arena) on a distributed stack while that stack page is
being pulled away just waits for the page like any other fault. The
cross-machine cycle that R1-R3 rule out for the runtime would need the
service thread to contend for the same lock, which it does not for stdio and
- with glibc's per-thread arenas - does not in practice for malloc.

## Migration

A context can move between machines at a **migration point**
(`dnuma_migration_point()`), cooperatively:

1. The master sends `EXEC_SUSPEND`; the runtime sets a flag.
2. At its next migration point the fiber switches back to its carrier, which
   reports `EXEC_SUSPENDED` with the saved stack pointer and ends.
3. The master picks the destination (falling back to the current machine if
   the requested one is no longer usable), ensures an image there, and sends
   `EXEC_MIGRATE` with that stack pointer.
4. A new carrier on the destination switches to it. Its first stack accesses
   fault the stack pages across. The call to `dnuma_migration_point()` returns
   on the new machine.

The state that moves is exactly what the System V ABI makes callee-saved at a
call boundary - `rbx`, `rbp`, `r12`-`r15`, MXCSR and the x87 control word - plus
the stack; all of it is *on the stack* (`ctx_switch.S`), so the only thing in
the message is one pointer and the memory system does the transfer. Vector and
x87 data registers are caller-saved at a call, so no XSAVE area is needed for
this kind of migration. Images share one stack-protector canary so frames
created on one machine verify on another.

What does not move: thread-local storage. A fiber sees its *carrier's* TLS, so
after a migration `errno` and `__thread` variables are different objects. Code
must not keep TLS-derived pointers across a migration point.

For threads created with `DNUMA_NODE_ANY`, the pager identifies the faulting
execution context when the kernel supplies its TID. A committed write that
takes a page away from a recent remote accessor contributes its measured
transaction time toward moving the two contexts together. Read replication
alone contributes nothing. The master ages these observations with a
one-second half-life and requests a move when repeated handoffs cost more
than the observed median migration, initially estimated at five milliseconds.
The destination must accept work and have space for another execution
context. The move takes effect at the worker's next migration point.
Explicit node selection and successful manual placement exclude a context
from automatic locality moves. `locality_migration_requests` counts the
requests and `thread_migrations` counts completed moves.

Used by drains (`lifecycle.md`) and by `dnuma_thread_migrate()`. Measured in
the drain test: two contexts moved from the slave to the master mid-computation and
finished with correct results (`thread_migration_ns` in `dnumactl metrics`).

**Not implemented:** preemptive migration (moving a thread at an arbitrary
instruction). Design: deliver a signal to the carrier; the kernel writes the
complete user context, including the XSAVE area, into the signal frame on the
context's distributed stack; suspend inside the handler; on the destination
resume with `rt_sigreturn` from that frame. It requires identical XSAVE layout
on both machines - the agent already reports `xcr0` and the XSAVE area size
(here `0x7`, 832 bytes on both) so the master can check - and TLS that moves
(below).

## System calls

A system call made by a remote thread enters the *slave's* kernel, in the
shadow image's native context: it sees the shadow's descriptor table, PID and
futex queues, none of which belong to the logical process. dnuma's mechanism:

`dnuma_syscall(nr, a0..a5)` called on a slave sends `SYSCALL_REQUEST` through
the agent to the master, which routes it to the **home image**. There, a
dedicated **proxy thread** for that execution context performs the system call
and returns the result and errno. So the call executes in the logical
process's authoritative Linux context - not in the master daemon, which is a
different process with different descriptors and credentials. In the home
image `dnuma_syscall` is a direct system call.

* One proxy thread per remote context, so a blocking call (a futex wait)
  blocks only its own context, and the proxy is that context's stable identity
  in the master's kernel.
* Pointer arguments must point into distributed memory. They are then valid
  in the home image at the same address, and the home kernel's access to them
  is an ordinary (kernel-mode) userfaultfd fault that pulls the pages over.
  This is why images need a userfaultfd that handles kernel-mode faults.

Tested (`t_exec`): forwarded `getpid` returns the home PID on every node while
the native call returns the shadow's; forwarded `write` on a descriptor that
exists only in the home image, from a buffer written on the slave, lands in
the file.

**Futexes and synchronization.** `dnuma_futex_wait/wake` forward `futex(2)` to
the home kernel, which therefore sees every waiter of the logical process on
one queue (a futex on private anonymous memory is keyed by address, which is
the same everywhere). `dnuma_mutex` is a three-state futex mutex whose atomic
operations act on a distributed page: since a page is writable on one machine
at a time and changes hands only between instructions, `lock`-prefixed
operations on it are atomic across machines. Tested in `t_coherence`
(atomic counter and mutex-protected counter, threads on both machines).

**Not implemented:** automatic interception. An unmodified `write(2)` or
`pthread_mutex_lock` on a slave still goes to the slave's kernel. Design:
syscall user dispatch (`PR_SET_SYSCALL_USER_DISPATCH`, Linux 5.11+, available
on both machines) or a seccomp user-notification filter in shadow images, with
a per-call policy table - forward (descriptors, futex, signals, process
state), execute locally (`getcpu`, `sched_yield`, clocks), or emulate against
master state (`gettid`).

## Native Linux threads and Go

A Go process creates ordinary Linux threads. Its runtime schedules goroutines
on those threads. Medusa's native exporter currently moves one task with its
home task stopped. If a sibling were allowed to run at home, it could change
a heap page after the remote body fetched it. The export path therefore
requires a single-threaded process.

Master kernel patch 7 can attach to a native address space and page existing
private anonymous memory. In the VM tests, a pthread and a Go process exchange
ownership of a heap page through the master's directory. The Go process uses
the stock runtime and runs garbage collection during the test.

Cross-host execution of one native thread group needs the following work.

1. Connect the exporter and the agent's native bodies to one directory per
   address space. Track `mmap`, `munmap`, `mprotect`, `brk`, address reuse
   and writable aliases, including changes initiated by a sibling.
2. Place individual Linux TIDs and observe `clone` and `clone3` so new
   threads inherit placement and process membership. Preserve TLS, extended
   registers, signal masks, `rseq`, robust lists and clear-child-TID state
   when a thread moves or exits.
3. Keep a native thread placed remotely while a private futex wait blocks
   at home. Private wake and bitset wake already use the home kernel.
   Deliver thread-directed signals to the intended TID, including Go's
   asynchronous preemption signal. Coordinate group stop, exec and exit.
4. Apply Linux affinity masks to placement, confirm execution on a permitted
   CPU, and keep the mask across migration. Page ownership changes identify
   threads that contend for the same writable page. The placement policy
   can use that evidence to move either thread to an allowed host.

`build/tests/n_thread_pair <cpu-a> <cpu-b>` exercises the ordinary Linux
interface. Two pthreads pin themselves, check their actual CPUs and TIDs,
and alternate updates to a shared heap using private futexes. Running on two
local CPUs provides the baseline. Passing a local and a remote CPU is the
cross-host acceptance test. The current remote affinity setter returns
`EINVAL` for a remote-only mask on the home kernel.
