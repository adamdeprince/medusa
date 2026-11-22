# dnuma - distributed asymmetric NUMA over RDMA

One authoritative Linux machine (the **master**) plus dynamically attachable
**slaves** that contribute CPUs and memory, connected by InfiniBand. There
are two ways to use the slaves:

* **Ordinary single-threaded programs, unchanged.** A running process on the master is
  *exported*: its code executes on a slave CPU while the process itself -
  PID, files, terminal, signals - stays on the master, which executes
  forwarded system calls. Nothing is installed on the slave but the agent, and
  no shared file system is needed. With `--auto-export` the master does this
  for eligible CPU-bound processes when local CPUs are oversubscribed and a
  slave has room. [Process export](docs/export.md)
* **Programs written for it.** With `libdnuma`, threads of one logical
  process run on any machine and share one virtual address range; pages
  follow the accesses, moved between machines by RDMA under a single
  ownership directory on the master. If two automatically placed threads
  repeatedly take write access to the same page, the master can move them
  closer together at a cooperative migration point. [Thread placement](docs/execution.md#migration)

Slaves run stock kernels. The master can run a stock kernel for both modes.
Optional master patches display slave CPUs in `top` and `/proc/cpuinfo`,
report a confirmed remote pin, and let a pager attach to a live native heap.
The native pager has been tested with pthreads and Go in a VM. Distributing
the Linux threads of one Go process still requires integrating that pager with
per-thread placement. [Kernel tests](kernel/README.md) · [Native thread support](docs/execution.md#native-linux-threads-and-go)

Built against **Linux 7.3.5 UAPI headers**; kernel features are detected at
run time (slaves on 5.15 and 7.0 have been used). `docs/architecture.md`
lists the machines it has been tested on.

## What it looks like

A master with 96 CPUs of its own, running a kernel with the view patches,
and one slave offering 24 CPUs. 120 CPU-bound copies of a test program are
started the ordinary way (`dnuma-master --auto-export` is running):

```
$ seq 120 | xargs -P 120 -I{} build/tests/n_spin 20 8 /tmp 50 &
$ grep -c ^processor /proc/cpuinfo
120
$ ps -C n_spin -o psr= | awk '{ if ($1 >= 96) r++; else l++ } END { print l " on local CPUs, " r " on CPUs 96-119" }'
96 on local CPUs, 24 on CPUs 96-119
$ ps -C n_spin -o pid,psr,stat,pcpu,time,comm | sort -k2 -n | tail -3
  83892 117 R<   81.6 00:00:08 n_spin
  83891 118 R<   83.7 00:00:09 n_spin
  83889 119 R<   78.4 00:00:08 n_spin
```

and from `top` (per-CPU rows) during such a run - CPU 94 is the master's,
96 and up are the slave's:

```
top - 17:42:32 up  1:10,  1 user,  load average: 24.17, 16.09, 9.24
Tasks: 1130 total, 118 running, 1008 sleeping,   4 stopped,   0 zombie
%Cpu94 : 94.9 us,  5.1 sy,  0.0 ni,  0.0 id,  0.0 wa,  0.0 hi,  0.0 si
%Cpu96 : 88.3 us,  2.4 sy,  0.0 ni,  9.3 id,  0.0 wa,  0.0 hi,  0.0 si
%Cpu98 : 87.9 us,  1.6 sy,  0.0 ni, 10.5 id,  0.0 wa,  0.0 hi,  0.0 si
%Cpu100: 89.8 us,  0.8 sy,  0.0 ni,  9.3 id,  0.0 wa,  0.0 hi,  0.0 si
```

All 120 programs check their memory on every round and exited 0. These are
copies of real output from the test machines, trimmed; they show that it
works, not how fast it is (`docs/testing.md` has the measurements there
are). The processes on CPUs 96-119 never left the master: their pids,
files and terminal are there, and `kill`, `ps` and job control work on
them as on any other process.

## What works today (tested on real machines)

* Process export: unmodified, dynamically linked, single-threaded programs
  moved to a slave while running and back (on request, on a signal, when
  they fork or exec, when the slave is drained); about 120 system calls
  forwarded with their data and executed by the original process; clock
  reads served on the slave from a measured offset; an automatic balancer
  (`seq 200 | xargs -P 200 ...` on a 96-CPU master spreads onto the slave).
* A slave can offer part of itself (`dnuma-agent --nodes 3`): the agent and
  everything it hosts stay on those NUMA nodes' CPUs and memory.

* Attach a slave while the master runs; mutual authentication; real hardware
  discovery (sockets, NUMA nodes, memory, HCAs and their NUMA locality,
  userfaultfd features); stable slave identity and per-attachment incarnation;
  logical NUMA numbering that never changes for a running system.
* RDMA RC links with per-link protection domains, bounded registered page
  pools, READ/WRITE verified at every attach, direct slave-to-slave transfers.
* One distributed address range per process; 4 KiB coherence with exclusive
  ownership, read-only sharing, invalidation (unmap verified through pagemap),
  migration, eviction to a machine, populate, safe abort of interrupted
  transfers; pages placed on the requested physical NUMA node.
* Real code executed on the slave's CPUs, with placement verified from the
  kernel on that machine; cooperative migration of running threads between
  machines; system calls forwarded to the process's home context on the
  master; a mutex and futexes that work across machines.
* Graceful drain that keeps dependent processes running; hard disconnect with
  dry run, confirmation and deterministic kill of dependents only; failure
  detection by connection loss, RDMA loss and lease expiry; fencing with an
  explicit "not proven stopped" state.
* `dnumactl` for all of it, metrics with latency distributions, per-page
  ownership tracing.

## What it is not (yet)

* Export moves whole single-threaded processes. A multi-threaded program is
  refused; so is one with a writable shared mapping. A process runs on one
  machine at a time, and each forwarded system call costs a network round
  trip, so it pays off for work that computes more than it calls.
* Ordinary pthread programs do **not** run *across* machines. That needs the
  `libdnuma` API (`dnuma_alloc`, `dnuma_thread_create`, ...) and a static
  non-PIE build; only `dnuma_alloc()` memory and context stacks are
  distributed, and thread migration happens only at
  `dnuma_migration_point()`.
* The Linux scheduler operates on local CPUs. The view patches display
  remote execution on the master. An explicit `dnumactl export PID --cpu ID`
  pins a native body on the agent. Optional patch 6 reports the confirmed
  CPU through `sched_getaffinity()`. Ordinary `sched_setaffinity()` still
  requires a local CPU in the mask. [Process export](docs/export.md)
* One physical slave at a time has been used. Multi-slave logic is covered
  by unit simulation and by a second agent co-located on the master's host.
* One master, state in memory, no restart recovery; management traffic is
  authenticated but not encrypted.

`docs/architecture.md` has the full status table.

## Build

```
scripts/kernel-headers.sh                       # fetch + verify Linux 7.3.5, install UAPI headers into .deps/
cmake -S . -B build -G Ninja \
      -DDNUMA_KERNEL_HEADERS=$PWD/.deps/linux-7.3.5-headers/include
cmake --build build
ctest --test-dir build                          # unit tests, no hardware needed
```

Requires g++ 11+ (C++20), CMake 3.20+, libibverbs and librdmacm development
files (rdma-core). libnuma is not needed. `-DDNUMA_SANITIZE=address` builds
the daemons and unit tests with a sanitizer.

Outputs: `dnuma-master`, `dnuma-agent`, `dnumactl`, `dnuma-run`,
`libdnuma.a`, `examples/dnuma-hello`, test programs in `build/tests/`.

## Run

Both daemons need root (userfaultfd for other processes, pagemap, pidfd
signalling, memory pinning). A shared cluster key authenticates them.

```
# on every machine: the same key file, mode 0600
head -c 48 /dev/urandom | base64 -w0 > /etc/dnuma/cluster.key

# slave (management on :7710, RDMA on :7711; --nodes offers only some NUMA nodes)
dnuma-agent  --key-file /etc/dnuma/cluster.key [--nodes 1 --capacity 16G]

# master
dnuma-master --key-file /etc/dnuma/cluster.key [--auto-export]
dnumactl slave connect node-b --address 192.0.2.10       # a name of your choice, the slave's address
dnumactl topology

# an ordinary program, moved while it runs
sha256sum big.iso &  dnumactl export $! node-b
dnumactl process list;  dnumactl recall <pid>

# a program written for dnuma
dnuma-run ./build/examples/dnuma-hello                  # as an ordinary user
```

`scripts/cluster.sh start|stop|status|logs` does the daemon part on a set of
machines named in a site file (`scripts/dnuma-env.example`; nothing in dnuma
itself knows any host name or address); `examples/demo.sh` runs the complete
demonstration.

### dnumactl

```
dnumactl slave list
dnumactl slave status <name>
dnumactl slave connect <name> [--address A] [--port P]
dnumactl slave disconnect <name> --drain [--timeout 300s]
dnumactl slave disconnect <name> --hard [--dry-run] [--yes]
dnumactl slave cancel-drain <name>
dnumactl topology
dnumactl memory status [--slave <name>]
dnumactl process list  [--slave <name>]
dnumactl metrics       [--slave <name>]
dnumactl export <pid> [<name>] [--cpu <cluster-cpu>]  # CPU IDs appear in slave status
dnumactl recall <pid> | --all               # bring exported processes home
dnumactl fault <name> <pull-delay ms | drop-rdma | partition ms | exit>   # testing only
```

Exit status: 0 success; 1 operation failed (for a drain: the slave is still
attached and nothing was lost); 2 usage or refused; 3 hard disconnect done but
fencing incomplete; 125 master unreachable.

### Writing a distributed program

```cmake
dnuma_add_executable(myprog myprog.cc)     # static, non-PIE, linked with libdnuma
```

```c
#include "dnuma/dnuma.h"

static uint64_t work(void* p) { /* runs on the chosen NUMA node's machine */ }

int main(void) {
    uint64_t* data = dnuma_alloc(1 << 20, 5, 0);   /* placed on logical node 5 */
    dnuma_thread_t t;
    if (dnuma_thread_create(&t, 5, work, data) != 0)   /* fails rather than run elsewhere */
        fprintf(stderr, "%s\n", dnuma_last_error());
    dnuma_thread_join(t, NULL, NULL);
    /* data[] is readable here by plain loads */
}
```

See `include/dnuma/dnuma.h` and `examples/hello.cc`.

## Tests

```
tests/run_all.sh                  # unit, integration and failure suites
DNUMA_ASAN=1 tests/run_all.sh
kernel/test/run.sh <bzImage>      # the kernel view, in a virtual machine
```

The integration and failure tests need a master and one slave, named in
`.dnuma-env` (see `scripts/dnuma-env.example`).

`docs/testing.md` lists what each test verifies, the results, and measured
latencies.

## Layout

```
include/dnuma/        public C API
src/common/           reactor, logging, metrics, hashing, hardware discovery
src/transport/        wire format, authenticated streams, RDMA links
src/node/             per-machine pager and execution host (used by master and agent)
src/master/           registry, lifecycle, topology, page directory, processes, control,
                      home side of exported processes, the kernel view
src/agent/            slave service; bodies of exported processes
src/export/           system-call descriptors and messages of process export
src/runtime/          libdnuma
src/cli/              dnumactl, dnuma-run
kernel/               kernel patches, their virtual-machine test, integration plan
tests/unit|integration|failure
examples/             hello program, end-to-end demo
docs/                 architecture, export, protocol, memory, execution, lifecycle, testing
scripts/              kernel headers, build sync, cluster helpers, one-time kernel boot
```

## Licence

GNU General Public License, version 2 only (`LICENSE`; SPDX
`GPL-2.0-only`). The kernel patches in `kernel/` are GPL-2.0, like the
kernel they apply to.

## How this was written

The code, tests, kernel patches and documentation in this repository were
written with AI coding assistants under the direction of the
repository's owner, on the owner's machines. Every statement about what
was tested refers to a run that actually took place; `docs/testing.md`
says which, and what has not been tested.
