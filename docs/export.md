# Process export: ordinary programs on slave CPUs

`dnuma-run` programs are written for dnuma. Process export is the other way
in: an ordinary, unmodified, dynamically linked program that was started on
the master in the usual way is moved to a slave **while it runs**, and comes
back when it has to. Nothing is installed in the program and nothing has to
exist on the slave except the agent: no shared file system, no copy of the
program, no user accounts.

```
$ sha256sum /data/big.iso &            # an ordinary process on the master
$ dnumactl export $! slave1            # now computing on slave1; the file is still read here
$ dnumactl slave status slave1         # "Cluster CPUs" gives the range to use
$ dnumactl process list
$ dnumactl recall <pid>                # or: it simply finishes
```

With `dnuma-master --auto-export` no command is needed: when the master has
more runnable work than CPUs, it moves CPU-bound processes to idle slave
CPUs by itself (`seq 200 | xargs -P 200 ...` on a 96-CPU master spreads onto
the slaves).

## What "exported" means

The process (call it P) never leaves the master. It keeps its pid, parent,
session, terminal, open files, sockets, working directory and credentials,
and signals are sent to it as always. What moves is the *execution of its
user code*: on the slave the agent creates a **body**, an empty process that
receives P's registers and, page by page as it touches them, P's memory. P
itself sleeps on the master and wakes only to do what its body cannot do
remotely.

| | Where it happens |
|---|---|
| User-mode instructions | slave CPU (the body) |
| Memory | pulled from P on first touch, in batches that grow with sequential access (64 KiB up to 1 MiB); pages the body modified are written back to P when it comes home |
| System calls | **executed by P itself on the master** (see below) |
| `clock_gettime`, `gettimeofday`, `time` | answered on the slave from its clock and a measured offset to the master's (see *Time*) |
| Signals | delivered to P on the master; P's body is brought home first, so handlers, job control and core dumps behave as usual |
| `fork`, `clone`, `execve`, and any call without a descriptor | the process comes home and the call runs natively |
| Exit | the body exits, P exits with the same status |

An administrator can select the exact slave CPU with
`dnumactl export <pid> --cpu <cluster-cpu>`. The agent sets the Linux CPU
affinity of the body before executing its first instruction. The selection
lasts for this export. With optional master kernel patch 6,
`taskset -pc <pid>` and `sched_getaffinity()` report the acknowledged pin
during that interval. Recall restores the home affinity report.

Read-ahead transfers neighboring pages with write protection. A store to the
faulting page requests write access to that page alone. A later store to a
prefetched page takes a write-protection fault. Syscall output also checks
the application's VMA permissions before the agent writes into a page.
Only modified pages need to travel home.

### System calls

Every system call the body makes stops it (ptrace `PTRACE_SYSEMU`, so the
call is never executed by the slave's kernel). The agent looks the call up
in a table (`src/export/sysdesc.cc`, x86-64) that says what each pointer
argument points at, gathers that data from the body, and sends it to the
master. There P is made to execute the real system call - in its own
process context, with its own descriptors and credentials - and the results
and output buffers travel back. `read(fd)` on an exported process therefore
reads the master's file with the master's page cache; a `write` to a
terminal appears on the master's terminal.

Calls fall into five classes:

* **Forward** (about 120 calls: file, socket, pipe, poll/epoll, stat, time,
  resource, id calls...): executed at home as described.
* **Memory** (`mmap`, `munmap`, `mprotect`, `brk`, `madvise`): executed at
  home so that P's address space stays the truth, and mirrored on the body.
* **Local** includes `arch_prctl`, `getcpu` and `sched_yield`, answered on the slave.
* **Exit**.
* **Home**: everything else. A call that is not described is never guessed
  at: the process is brought home and runs the call natively. It may be
  exported again afterwards.

`FUTEX_WAKE_PRIVATE` and `FUTEX_WAKE_BITSET_PRIVATE` run in the original
task at the original virtual address. Linux selects its private futex queue
using the address and home address space. A native futex wait currently
brings execution home so Linux can inspect the latest word.

### What can be exported

A process is refused (and left exactly as it was) if it has more than one
thread, is not a 64-bit process, has a shared mapping whose backing can change elsewhere, or uses the address range
the body stub occupies. A slave is refused as a target if its CPUs lack any
instruction-set feature of the master's (a process may already have chosen
code paths by them) or use a different extended register state layout.

Read-only shared memfd mappings qualify when the backing has permanent
write, grow and shrink seals. The master checks the mapped file, so closing
the original fd does not affect export. A mapping with an external writable
alias requires live invalidation. The exporter declines such processes and
recalls an already exported process before it creates a mutable shared
mapping. Private writable mappings use per-page dirty tracking.

The automatic balancer only considers processes that are single-threaded,
runnable, measurably CPU-bound over its last interval, and owned by a uid at
or above `--export-uid-min` (default 1000). A process with a restricted home
CPU affinity mask stays home. The master checks its mask again after stopping
it for export. A process comes home by itself or when its slave is drained.

### Coming home

A process comes home when a signal arrives for it, when it makes a call of
class Home, when it faults (the fault is re-raised at home so that the right
signal is produced), on `dnumactl recall`, when its slave is drained
(`slave disconnect --drain` recalls every exported process first and then
proceeds as before), and when the master shuts down. Its modified pages and
registers are written back into P, and P continues as an ordinary local
process.

If the slave is lost or hard-disconnected, the memory the body had modified
is gone. Those processes - and only those - are killed, as for any other
work that depended on a lost slave; `--hard --dry-run` lists them.

### Time

Reading the clock is the most frequent system call of many programs, so it
is not forwarded. The agent compares clocks with the master once a second
(an NTP-style exchange over the operations link) and keeps the best recent
sample; `clock_gettime(REALTIME|MONOTONIC|BOOTTIME|TAI|MONOTONIC_RAW)` and
the coarse variants, `gettimeofday` and `time` are answered from the slave's
clock plus that offset. The error is bounded by half the exchange's round
trip plus drift since the sample. Two guarantees are kept exactly: a clock
never goes backwards for the process (neither when it is exported nor when
it comes home - the master delays its return by the few microseconds its
own clock may be behind), and wall clocks follow the master's when it is
set. The vDSO of an exported process is patched so that these calls reach
the kernel interface instead of reading the slave's clock directly.

## Offering part of a machine

A slave that has other work can offer only some of its NUMA nodes:

```
dnuma-agent --nodes 3 --capacity 16G ...
```

The agent then describes only those nodes to the master (their CPUs, their
memory, the sockets they belong to), and before it creates any thread or
process it restricts itself to those nodes' CPUs (`sched_setaffinity`) and
memory (`set_mempolicy(MPOL_BIND)`). Everything it hosts - bodies and
`dnuma-run` images - inherits both, so nothing of dnuma runs or allocates
outside the offered nodes. What it cannot move are the kernel's own costs:
interrupts of the network card are handled where the card is, and page cache
on the offered nodes is reclaimed as dnuma's memory grows.

## Seeing it: the kernel view

With an unmodified kernel on the master an exported process looks idle
(it sleeps while its code runs elsewhere) and the slave's CPUs are invisible.
A small kernel change on the **master only** (`kernel/patches/`,
`CONFIG_DNUMA_VIEW`, see `kernel/README.md`) lets the master describe both
to its kernel. While `dnuma-master` runs on such a kernel:

| | Shows |
|---|---|
| `/proc/cpuinfo` | the slaves' CPUs after the master's own, numbered from the first number the master could never use itself; each block is the slave kernel's own text for that CPU, with a `dnuma host` line naming the machine |
| `/proc/stat` | a `cpuN` line for each slave CPU with that CPU's real counters (sampled on the slave twice a second), included in the `cpu` total |
| `/proc/<pid>/stat`, `/proc/<pid>/status` | for an exported process: the slave CPU it is on, state `R` while its body is on a CPU (at the last sample, or for at least half of the interval before it), and its CPU time including what it has used on slaves (kept after it comes home) |
| `/sys/devices/system/cpu/online` | the slave CPUs as well (not with `--view-proc-only`) |

So `grep -c processor /proc/cpuinfo` counts the cluster, `top` lists exported
processes as running with their real CPU usage and (with the `P` column) on a
slave CPU, and its per-CPU display has a row for every slave CPU.

The last row of the table has a wider effect than the others, and it is the
reason it can be turned off. The C library takes the number of online
processors (`sysconf(_SC_NPROCESSORS_ONLN)`, `getconf _NPROCESSORS_ONLN`)
from that file. Older versions of `top` (procps 3.x) show only that many
per-CPU rows, which is why it is on by default; but programs that size
their thread or process pools by it (Python's `os.cpu_count()`, for example)
will also start more workers than the master has CPUs. With `--auto-export`
that is what puts the slaves to use; without it the extra workers share the
master's CPUs.

What does **not** change, deliberately:

* With the original five kernel patches, the scheduler, CPU masks and affinity know only the master's CPUs.
  `nproc` (which counts the CPUs the caller may run on) and
  `sched_getaffinity` report the master's; a slave CPU cannot be the target
  of `taskset`. `lscpu`, which reads per-CPU sysfs directories, describes
  the master. Patch 6 adds the affinity query for a confirmed remote body.
  Selecting a remote CPU through `sched_setaffinity()` still requires a
  kernel scheduling backend.
* The kernel's own accounting. `getrusage`, `wait4` (and so `time cmd`) and
  cgroup CPU accounting contain only what ran on the master; the remote time
  is visible in `/proc/<pid>/stat` only.
* Everything is display. The view is written by the master through
  `/proc/dnuma_view` and vanishes when the master exits for any reason.

Inside an exported process `sched_getcpu()` and `getcpu()` return the same
slave CPU number that `/proc/<pid>/stat` shows - a number at or above the
master's CPU count. A program that indexes a table of `sysconf(_SC_NPROCESSORS_CONF)`
entries with it would be out of bounds; none was met in testing, but the
possibility is real.

## Running the daemons at higher priority

The service threads of both daemons run at nice -20: a forwarded call or a
page request must not queue behind the CPU-bound work the system exists to
run. `--realtime` puts them in `SCHED_FIFO` priority 1 instead. That removes
the remaining scheduling latency, at the usual price of a real-time class: a
service thread that spins is not preempted by ordinary work. It is off by
default.

The same applies to an exported process itself. What is left of it on the
master only executes forwarded system calls, so while it is away it is given
the same priority as the service threads, and `top` shows it with `NI -20`
(or in the real-time class with `--realtime`). Its own scheduling policy and
nice value are saved at export and restored when it comes home.

## Status

| | |
|---|---|
| Export, recall, signals (TERM, STOP/CONT), exit status, forwarded I/O on master-only files | implemented; tested (`tests/integration/07_export.sh`) |
| Automatic balancer with `xargs -P` over-subscription | implemented; tested (`08_export_balancer.sh`) |
| Drain recalls exported processes; hard disconnect kills exactly them | implemented; exercised by the drain and hard-disconnect tests only for `dnuma-run` programs - not yet with exported processes in the automated suite |
| `--nodes` confinement | implemented; verified by inspection on the test slave (thread affinity, `numa_maps`, CPUs bodies ran on) and by running the whole suite against a one-node agent |
| Kernel view: kernel side | implemented; tested in a virtual machine, including under lockdep and KASAN (`docs/testing.md`) |
| Kernel view: master and agent side | implemented; tested end to end on a master running the patched kernel (`09_kernel_view.sh`), and the master's client separately against the kernel interface |
| Multi-threaded processes, writable shared mappings, 32-bit processes | not supported (refused) |
| Export of a process between slaves, or from a slave back out | not implemented: a process is either at home or on one slave |
| Encryption of memory and system-call data in transit | not implemented (authenticated links only) |

Measured throughput is in `docs/testing.md`.
