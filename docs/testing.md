# Testing

Everything below is run by `tests/run_all.sh` on the master host. Tests check
memory contents, executing host and CPU, physical placement, process survival
and exit status - not just that a request returned success.

```
tests/run_all.sh                 # unit + integration + failure
tests/run_all.sh unit            # no hardware needed (also: ctest in build/)
DNUMA_ASAN=1 tests/run_all.sh    # daemons and unit tests under AddressSanitizer
examples/demo.sh                 # the 12-step end-to-end demonstration
kernel/test/run.sh <bzImage>     # the kernel view, in a virtual machine
```

Setup assumed by the scripts: a site file, `.dnuma-env` (template:
`scripts/dnuma-env.example`), naming the slave and its address - no machine
name or address is built into any script; the built tree in `build/` visible
under the same path on the slave (shared file system or a copy); passwordless
`sudo` and ssh from the master to the slave. `scripts/cluster.sh
start|stop|status|logs` runs the daemons (a cluster key is generated on first
use in `.deps/`). If the site file gives the agent `--nodes`, the tests
compare the master's view with those nodes only.

## Unit tests (`tests/unit`, 33 tests)

| File | What it checks |
|---|---|
| `test_common.cc` | SHA-256 and HMAC-SHA-256 against FIPS/RFC vectors; size, duration and cpulist parsing including overflow; histogram quantiles and bucket bounds; metrics encoding |
| `test_wire.cc` | Message round trips; every truncation of a payload and any trailing byte is rejected; hostile element counts cannot force allocation; header validation; authentication proofs resist key mismatch, reflection and identity substitution |
| `test_lifecycle.cc` | Every documented transition; illegal ones rejected; a failed drain has no path to fencing; an unproven fence cannot be bypassed; NodeIds and logical numbers stable across detach/reattach and unaffected by other slaves |
| `test_coherence.cc` | The page directory against simulated machines (below) |
| `test_kernel_view.cc` | The master's kernel-view client: parsing and rewriting of `/proc/cpuinfo` blocks; and, when run as root on a kernel that has the view, the live interface - what `/proc/cpuinfo`, `/proc/stat`, `/proc/<pid>/stat`, sysfs `online` and `sysconf` show after each command, a batch larger than one write, and that everything disappears on close. On other kernels that part reports "not run" |

`test_coherence.cc` drives the real coherence engine with one simulated master
and three simulated slaves. Messages are delivered in random interleavings
(FIFO per channel, as the transports guarantee); after **every** delivery it
checks that no two machines hold write access to a page, that a writer never
coexists with another copy, and that every machine that can read a page reads
the latest value written. 12 seeds x 6000 steps mix read and write faults,
local writes, migrations, replica drops and injected out-of-space failures,
then compare the directory and its usage counters with what the machines
actually hold. Deterministic cases cover: write access granted only after all
three other holders acknowledge; slave-to-slave transfer without the master
holding a copy; destination dying during the revoke phase and during the grant
phase (source keeps the page, process not blamed); destination out of space;
ownership hand-off without data transfer; stale and duplicate messages;
allocation, free and reuse; process destruction cancelling transfers.

This is where behaviour with several independent slaves is tested most
thoroughly. It is a simulation of the node side, not physical machines.

## Integration tests (`tests/integration`, both machines)

| Script | Scenario and what is verified |
|---|---|
| `01_attach.sh` | Attach while the master runs. CPU count, NUMA node count and per-node memory in the master's view are compared with the slave's own `nproc`, sysfs and meminfo. Repeated connect is a no-op with unchanged incarnation; unreachable host fails cleanly; the same agent under a second name is refused. |
| `02_programs.sh` | `t_memory`: pages written on the slave are on the requested *physical* NUMA node according to the slave's kernel (`move_pages` query); contents verified on the master; directory states checked at each step (EXCLUSIVE on the slave -> SHARED -> EXCLUSIVE on the master after a write -> SHARED after the slave reads back); explicit migration; populate; placement on a nonexistent node refused. `t_exec`: a function runs on every NUMA domain of both machines; host and physical node checked from inside the function and from the hosting daemon; partial sums combined and checked; forwarded `getpid` equals the home PID while the native one differs on the slave; forwarded `write` to a home-only descriptor. `t_coherence`: stale-read detector, atomic counter, same-page writers and a mutex, with threads on both machines. Then: no unverified revocation, no forced termination, every transition traced, nothing left running. |
| `03_drain.sh` | An application with 2 contexts and 64 MiB on the slave is drained while running. Verified: progress output; state DISCONNECTED with proof clean-detach; no page and no image left on the slave; placement on the slave now refused (not run elsewhere); the application keeps making progress on the master, finishes with every word equal to its pass count, reports 2 migrations; reattach gives a newer incarnation and the same node numbers. |
| `04_drain_nomem.sh` | Master capacity set to 16 MiB, 64 MiB on the slave: the drain fails with "insufficient memory elsewhere", slave still ACTIVE, nothing terminated, application finishes intact on the slave. Then an application without migration points: a 2 s drain times out naming the stuck context, slave still ACTIVE, application finishes intact. |
| `05_hard.sh` | A dependent application (the slave) and an unrelated one (the master only). Dry run lists exactly the dependent PID and changes nothing; non-interactive hard disconnect without `--yes` is refused; with `--yes` the dependent process exits with 137 (SIGKILL), its image and metadata are gone, fencing is agent-confirmed, the unrelated application finishes intact; repeat is a no-op; reattach works and runs new work. |
| `06_two_slaves.sh` | A second agent with its own identity runs on the master (see the caveat below). 12 NUMA domains; data written on the slave is read and rewritten on the second slave and read back on the slave, with pages pulled directly between the two slaves; `t_exec` across all 12 domains; a drain of the slave with the master nearly full evacuates to the other slave; killing the slave's agent kills only the slave's dependent while an application on the other slave finishes, and the survivor drops its link to the failed peer. |

| `07_export.sh` | Process export with an ordinary program (`n_spin`: CPU-bound, rewrites and verifies a 16-64 MiB buffer, does file I/O in a directory that exists only on the master). Export and recall while running: the program reports having run on a slave CPU and then on a master CPU again, memory verified throughout, exit code 0. SIGTERM while exported: exit status 143. SIGSTOP/SIGCONT while exported: state `T`, no progress while stopped, then completes. `sha256sum` of a 300 MB file that exists only on the master, exported mid-run: correct digest. Counters: system calls forwarded, pages served; nothing lost, no body left on the slave. |
| `08_export_balancer.sh` | Automatic export. `xargs -P` starts more CPU-bound processes than the master has CPUs (by default as many extra as the slave offers). Every process reports PASS; at least half of the excess ran on slave CPUs; none lost; nothing left behind on either machine. Prints rounds completed per process for those that stayed and those that were exported. |
| `09_kernel_view.sh` | Needs the kernel view series (`kernel/patches/`) on the master; otherwise reports "not run". With a slave attached: `/proc/cpuinfo` lists master plus offered slave CPUs, the slave's with its own model name and a `dnuma host` line; `/proc/stat` has their lines and their counters advance at the slave's clock rate; sysfs `online` and `getconf _NPROCESSORS_ONLN` count them; `top` has a row for the last slave CPU. For an exported process: `/proc/<pid>/stat` shows a slave CPU, state `R`, CPU time growing; `ps` agrees; `top` shows it using a CPU and shows that CPU busy. After recall: a master CPU again, and the remote CPU time is still counted. After drain, reattach and master stop: the view shrinks, returns with the same numbers, and is empty. |

## The kernel view in a virtual machine (`kernel/test`)

`kernel/test/run.sh <bzImage>` boots a kernel built with the view series in QEMU
with `view-test-init.sh` as init - no installation, and the host's kernel is
not involved. 53 checks: the control file's mode and single-writer rule;
rejected input (a local CPU number, statistics for an undefined CPU, an
unknown or incomplete command, an invalid state); `/proc/cpuinfo` order,
content and byte-identical output when read in 37-byte pieces; `/proc/stat`
lines and the total; replacement of a CPU keeping its counters; a task's
state, CPU and times in `/proc/<pid>/stat`, per-thread `stat` and `status`,
and as shown by the real `ps` and `top` binaries (procps 3.3.17); sysfs
`online` off, on, with ranges; top's per-CPU rows with and without it;
"home again" semantics; automatic removal of an exited task; 1500 rounds of
three concurrent readers against a writer adding, replacing and removing CPUs
and updating a task; `clear`; and that closing the writer restores every
file. It then runs `tests/unit/test_kernel_view` as root inside the guest.
The script fails on any failed check or any BUG/WARNING/KASAN/lockdep line
in the guest's kernel log.

## Failure-injection tests (`tests/failure`)

| Script | Injection | Verified |
|---|---|---|
| `f1_agent_death.sh` | `kill -9` of the agent (also stands in for a reboot: agent and images vanish, a new agent starts later) | Dependent killed (within ~30 ms); state FENCE_INCOMPLETE and reported as "not proven stopped" until the lease bound, then DISCONNECTED with proof lease-expired; no image outlives the agent; unrelated application intact; new agent reattaches with the same identity and node numbers. |
| `f2_rdma_loss.sh` | Agent destroys its RDMA link (`dnumactl fault <slave> drop-rdma`); then a hard disconnect while page transfers to the slave are artificially stalled (`pull-delay`) | Link loss detected, dependent killed, fence agent-confirmed. For the stalled transfers: the process, which has nothing on the slave yet, is not killed; the migration call returns an error; all data intact and still owned by the master; aborted transfers counted. |
| `f3_partition.sh` | Agent goes silent on every channel with no close or disconnect delivered (`fault <slave> partition`) | Master detects by lease expiry, kills the dependent, reports the slave as not yet safe, declares DISCONNECTED only after the lease bound. Independently, the cut-off agent fences itself on its own lease. Then the cut-off incarnation issues an RDMA READ of the master's pool with the queue pair and rkey it still holds: **denied** (`transport retry counter exceeded`). Reattach afterwards works. |
| `f4_cycles.sh` | 8 connect/disconnect cycles alternating drain and hard, with a workload in each | Strictly increasing incarnations, constant node numbering, no forced termination, master healthy. |

A real InfiniBand cable pull and a real reboot of the slave were not performed
(the brief forbids disrupting the machines); the injections above are the
controlled substitutes.

## Results

Latest run (master and slave B of `docs/architecture.md`, the agent offering
one NUMA node of 24 CPUs; the master booted into 7.3.5 with the kernel
view series), normal build: 5 unit programs (33 tests), 9 integration scripts
(178 checks) and 4 failure scripts (58 checks) - all pass, with no warning
from the master's kernel during the run. The same suite passes on that kernel
with both daemons and the unit tests built with AddressSanitizer, with no
sanitizer reports (each script also checks the daemon logs for them; that is
the one extra check per script in such a run). The sanitizer runtime is
linked statically, so the instrumented agent runs on a slave whose compiler
differs from the build machine's.

On a master without the patch the suite is the same except that
`09_kernel_view.sh` reports "not run" and checks nothing, and
`test_kernel_view` skips its live part; that configuration passed as well
(the run before the kernel was booted).

The kernel view in a virtual machine (`kernel/test/run.sh`): 53 checks pass
on the normal build of 7.3.5 with the view series and on a build with
`PROVE_LOCKING`, `PROVE_RCU`, `KASAN`, `DEBUG_ATOMIC_SLEEP` and `DEBUG_LIST`,
with nothing reported in the guest's kernel log; the client test passes
inside both guests. The debug build has only been run in that 4-CPU guest,
not on the master. Those runs used the sources as one change; the kernel
built from the five-patch series in `kernel/patches/` (same resulting tree,
every commit also built on its own with the option on and off) was run
through the 53 checks again on the normal configuration and passes.

The first end-to-end run on the real master found one defect, since fixed:
an exported process was shown as running only if its body was on a CPU at
the instant of the slave's sample, so a program making a system call at
about the sampling rate could appear asleep for half a second at a time
while using a whole CPU. The state now also counts as running when the body
used at least half of the interval since the previous sample.

### Process export

Rounds completed per process by `n_spin` (CPU-bound; each round rewrites and
verifies 8 MiB and every 25th does file I/O on the master) in
`08_export_balancer.sh`, 12 s per process, one run each, slave B (24 CPUs
offered, kernel 7.0):

| Processes started on a 96-CPU master | Exported | Stayed on the master | Exported |
|---|---|---|---|
| 100 | 16 | 945 | 1158 |
| 120 | 24 | 879 | 832 |

In the first row the master's CPUs are all busy (two hyperthreads per core)
while the slave's are mostly idle, which is why exported processes did more.
On slave A (kernel 5.15), with 96 processes exported at once and before the
last round of changes to the agent, exported processes completed roughly a
third of what local ones did (265-342 against about 1000 rounds in 14 s);
that machine failed before the comparison could be repeated, so how much of
the difference is the slave's kernel, the smaller number of bodies or those
changes is not known.

A single exported `n_spin` makes on the order of 200 system calls per
second (352 in 2.6 s and 357 in 1.5 s in one test log); a process that
mostly makes system calls gains nothing from being exported,
because each call is a network round trip plus a stop and restart of two
processes.

### `dnuma-run` programs (measured on slave A)

Measured with Connect-IB FDR10, 4 KiB pages; `dnumactl metrics`; latencies
are histogram quantiles, monotonic clock, measured on one machine each -
never differences of two machines' clocks:

| Quantity | Value |
|---|---|
| RDMA self-test at attach, 16 KiB WRITE / READ | 27-29 us / 22-26 us |
| RDMA READ of one page, idle link | p50 ~21-25 us |
| Remote read fault on the master, end to end (uffd event to mapped) | p50 ~82-98 us |
| Coherence transaction on the master (request to commit) | p50 ~80-260 us depending on contention |
| Drain of 64 MiB (16,254 pages) with 2 running contexts | 0.37 s |
| Context migration the slave -> the master (suspend request to running) | 2-5 ms |
| Failure detection after agent kill | ~30 ms (connection reset) |
| Failure detection under a silent partition | ~4-5 s (lease 5 s) |
| Stale-incarnation RDMA READ after fencing | denied after ~33 s of HCA retries |

These are observations from the test runs, not benchmarks.

## Caveats

* **One physical slave.** Multi-slave behaviour is demonstrated by the
  coherence simulation and by `06_two_slaves.sh`, whose second "slave" is an
  agent process on the master's host. The slave-to-slave RDMA link in that
  test does cross the fabric (slave <-> master), but three physical machines
  have not been run.
* **Kernel patches.** The remote `MADV_DONTNEED` patch (`kernel/untested/`)
  has never been booted. The view series has run on one real master, for the duration of the test
  runs above, booted by kexec; it has not been through long uptimes, CPU
  hotplug or suspend on real hardware.
* **Exported processes in drain and hard-disconnect tests.** Both paths are
  implemented, and the single-process tests cover recall and kill, but
  `03_drain.sh` and `05_hard.sh` still use only `dnuma-run` programs.
* **`--nodes`** was verified by inspecting the agent and its bodies on the
  test slave (thread affinity, `numa_maps`, the CPUs bodies ran on); there
  is no automated check of the confinement itself.
* The stale-read test cannot prove absence of stale mappings; it would detect
  one with high probability (billions of paired reads per run), and the
  pagemap verification is the actual mechanism.
