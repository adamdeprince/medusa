# Kernel work

dnuma runs on unmodified kernels: everything it needs is detected at run
time, and the same binaries have been run on Linux 5.15, 7.0 and 7.3. It is
**built against Linux 7.3.5 UAPI headers** (`scripts/kernel-headers.sh`
fetches the release from kernel.org, checks its SHA-256 against the published
list and runs `make headers_install`; CMake refuses to configure without
headers that define the userfaultfd interfaces it uses).

This directory holds what belongs in the kernel: two optional changes for
Linux 7.3.5, the test that boots the first one in a virtual machine, and
the integration plan beyond them. Slaves never need a patched kernel.

| | What it adds | State |
|---|---|---|
| `patches/` - the dnuma view, a series of five patches | slave CPUs and exported work in the master's `/proc` | tested in a virtual machine (normal and lockdep/KASAN builds) and end to end on a real master; see its *Status* |
| `untested/` - remote `MADV_DONTNEED`, one patch | lets the pager discard a page of another process itself | compiles in 7.3.5; **never booted** - kept apart from the series for that reason |

## `patches/`: slave CPUs and exported work in the master's `/proc`

**Problem.** An exported process (`docs/export.md`) stays on the master and
sleeps while its code runs on a slave. `top` on the master shows it idle,
and the CPUs it is using are not listed anywhere, so a user has no ordinary
way to see that the system is doing anything.

**The series.** A root-only control file, `/proc/dnuma_view`, through which
a manager describes remote CPUs (their `/proc/cpuinfo` text and `/proc/stat`
counters) and, per local task, the remote CPU it is on, whether it is
running there, and the CPU time it has consumed there. Hooks of a few lines
each make the kernel show that. `0000-cover-letter.patch` introduces the
series; the patches are in `git format-patch` form:

| Patch | Files | Change |
|---|---|---|
| 1 | `fs/proc/dnuma_view.c`, `include/linux/dnuma_view.h` (new), `fs/proc/Kconfig`, `Makefile` | the control file and the tables behind it (RCU for readers, one mutex for the writer); `CONFIG_DNUMA_VIEW` (x86-64, default off). Nothing is shown yet |
| 2 | `fs/proc/cpuinfo.c` | a `/proc/cpuinfo` iterator that runs the architecture's own and then appends the remote CPUs |
| 3 | `fs/proc/stat.c` | a `cpuN` line per remote CPU; remote counters added to the `cpu` total |
| 4 | `fs/proc/array.c` | `/proc/<pid>/stat`: state, CPU number, user and system time; `/proc/<pid>/status`: state |
| 5 | `drivers/base/cpu.c` | `/sys/devices/system/cpu/online` lists the remote CPUs too, if the manager asked (`online on`) |

Patch 5 is last because it reaches furthest (it changes what the C library
reports as the number of online processors, see `docs/export.md`) and can
be left out: `dnuma-master` sees that the control file has no `online` line
and says in its log that the sysfs part is unavailable. The series without
patch 5 builds; it has not been run.

The commits carry no `Signed-off-by`: they were written by an AI assistant
and nobody has certified the Developer's Certificate of Origin for them
(the kernel's rules reserve that for a person). `git am -s` adds yours.

Properties that were design requirements:

* **Display only.** No CPU mask, no scheduler structure, no accounting field
  is touched. Remote CPU numbers must be at or above `nr_cpu_ids`, so a
  number never means both a local and a remote CPU.
* **Off means off.** All hooks are behind one static key that is enabled
  only while a manager has the control file open for writing.
* **Cannot outlive the manager.** There is one writer; when it closes the
  file - or dies - everything it wrote is discarded. Entries of tasks that
  exit are dropped by the kernel.

The command format is documented at the top of `fs/proc/dnuma_view.c`. The
consequences of listing remote CPUs in sysfs `online`, and what the view
deliberately leaves alone (`nproc`, affinity, `getrusage`), are in
`docs/export.md`.

**Userspace side (implemented).** `dnuma-master` opens the control file at
start (`src/master/kernel_view.cc`, `master_view.cc`); on a kernel without
it, it says so in its log and runs as before. Agents sample their offered
CPUs' counters and their bodies' CPU times twice a second (`VIEW_SAMPLE`);
the master renumbers and forwards them. `--no-kernel-view` and
`--view-proc-only` turn the view, or just the sysfs part, off.

**Building and trying it.**

```
cd linux                                  # a git tree at v7.3.5
git am .../kernel/patches/000[1-5]-*.patch
# (a release tarball works too: for p in .../kernel/patches/000[1-5]-*.patch; do patch -p1 < $p; done)
cp /boot/config-$(uname -r) .config
scripts/config --enable DNUMA_VIEW --set-str LOCALVERSION -dnuma
make olddefconfig && make -j$(nproc) bzImage modules

# in a virtual machine first (boots the image with a test as init; needs
# qemu-system-x86_64, or DNUMA_QEMU set to a command that runs it):
.../kernel/test/run.sh arch/x86/boot/bzImage

# then on the master. Either install it the way you install kernels, or boot
# it once without touching the boot loader:
sudo make INSTALL_MOD_STRIP=1 modules_install
sudo mkinitramfs -o /var/tmp/initrd-dnuma $(make -s kernelrelease)
sudo .../scripts/kexec-kernel.sh arch/x86/boot/bzImage /var/tmp/initrd-dnuma --now
```

**Status.**

| | |
|---|---|
| Applies to | Linux 7.3.5 |
| Builds | every commit of the series builds without warnings (`W=1` on `fs/proc/` and `drivers/base/cpu.o`) with `CONFIG_DNUMA_VIEW` on and off |
| Style | `scripts/checkpatch.pl` on each patch: nothing about the code except "externs should be avoided in .c files" for the declaration of `cpuinfo_op`, which `fs/proc/cpuinfo.c` itself declares that way. It also reports the missing `Signed-off-by` and the non-kernel attribution trailers |
| Kernel side tested | In a 4-CPU KVM guest booting the kernel built from this series with `kernel/test/view-test-init.sh` as init: 53 checks of the control file, `/proc/cpuinfo`, `/proc/stat`, `/proc/<pid>/stat` and `status`, sysfs `online`, the real `ps` and `top` binaries (procps 3.3.17), 1500 rounds of concurrent readers against a writer adding and removing CPUs, and cleanup on close. Passed with nothing in the kernel log. The same sources, before they were split into five commits (the resulting tree is byte-identical), also passed on a build with `PROVE_LOCKING`, `PROVE_RCU`, `KASAN`, `DEBUG_ATOMIC_SLEEP` and `DEBUG_LIST` |
| Master's client tested | `tests/unit/test_kernel_view` run as root inside the same guest, against the live interface (including batches larger than one write) |
| End to end (master + slave + `top`) | `tests/integration/09_kernel_view.sh` on a master booted into 7.3.5 with these changes (by kexec, as above) and one slave offering 24 CPUs: 29 checks pass; the rest of the suite passes on that kernel too (`docs/testing.md`) |
| Upstream | Not submitted, and not proposed in this form: showing CPUs the scheduler does not have is acceptable for a system that is explicit about it, not as a general kernel feature |

## `untested/`: remote `MADV_DONTNEED` for a privileged pager

**Problem.** To revoke a page from a process, its pager must remove the PTE
(not merely write-protect it). On Linux 7.3.5 no interface lets one process do
that to another:

* `process_madvise()` accepts only `MADV_COLD`, `MADV_PAGEOUT`, `MADV_WILLNEED`
  and `MADV_COLLAPSE` for a remote `mm` (`process_madvise_remote_valid()`);
* `UFFDIO_MOVE` would do it atomically, but `userfaultfd_move()` returns
  `-EINVAL` unless `ctx->mm == current->mm`;
* `UFFDIO_RWPROTECT` (new in 7.3) makes pages inaccessible from outside the
  process, but cannot be registered together with write-protect mode on one
  VMA (they share the uffd PTE bit), and dnuma needs write-protect for
  read-only sharing.

So dnuma asks the image's own runtime to call `madvise(MADV_DONTNEED)` and then
verifies the result through `/proc/pid/pagemap`, killing the image if the
mapping is not gone in time. That is safe, but its liveness depends on the
image.

**Patch.** 22 lines in `mm/madvise.c`: allow `MADV_DONTNEED` through
`process_madvise()` on a remote `mm` when the caller passes
`ptrace_may_access(task, PTRACE_MODE_ATTACH_REALCREDS)` in addition to the
existing `CAP_SYS_NICE` and `PTRACE_MODE_READ` checks. A caller that may
attach with ptrace can already rewrite the target's memory, so this grants no
new power; it only makes discarding possible without injecting code. The
existing remote path (`vector_madvise(mm, ...)`) is already written for a
foreign `mm`, including the fallback from the per-VMA lock to `mmap_lock`.

**Userspace side (implemented).** When an image registers, its node daemon
probes `process_madvise(pidfd, MADV_DONTNEED)` on a reserved, never-used page
of the image. If the kernel accepts it, every later invalidation is performed
by the pager directly (`revoke_remote_zap` in the metrics, "revocation:
process_madvise (performed by the pager)" in the log), still followed by the
pagemap verification. On unpatched kernels the probe fails with `EINVAL` and
the cooperative path is used. No rebuild or configuration is needed either way.

**Status.**

| | |
|---|---|
| Applies to | Linux 7.3.5 (`mm/madvise.c`) |
| Compile check | `mm/madvise.o` builds without warnings in a 7.3.5 tree configured with the test master's running config (`olddefconfig`) |
| Booted / run | **No.** The probe-and-use path in the pager has only ever taken the "not supported" branch |
| Upstream | Not submitted. Earlier proposals for destructive remote advice were declined on security grounds; the ptrace-attach condition is the argument for this one |

To try it: apply to a 7.3.5 tree, build and boot that kernel on a machine,
start the daemons; the image-ready log line shows which mechanism is in use.

```
cd linux-7.3.5 && patch -p1 < .../kernel/untested/mm-madvise-*.diff
```

## Beyond the patches: integration plan

The userspace components were shaped so that each has a kernel counterpart.
In dependency order, with the 7.3.5 code each step touches:

1. **Pager in the kernel.** NodeCore's fault handling (`src/node`) becomes a
   fault handler for a new VMA type: on a missing or write-protect fault it
   asks the coordinator and installs the page, instead of round-tripping
   through userfaultfd and `UFFDIO_COPY`. Revocation becomes
   `zap_page_range_single()` / `change_protection()` on the target `mm`
   directly, which removes the cooperative unmap and the `process_vm_readv`
   copy. Touches `mm/memory.c` (`handle_pte_fault` paths), `mm/userfaultfd.c`
   as the model. The wire protocol and the master's coherence engine are
   unchanged.

2. **RDMA from the kernel.** Page pulls via the in-kernel verbs API
   (`ib_post_send` with `IB_WR_RDMA_READ`) into freshly allocated pages, so a
   pulled page is mapped without a copy. Same per-link protection-domain
   scheme as `src/node/link_table.cc`.

3. **Remote tasks.** A thread of the logical process on a slave becomes a
   real task there whose `mm` is the distributed address space (step 1), with
   a shadow task on the master holding its place in the thread group, as in
   Popcorn Linux. System calls that need the master's context are forwarded
   from the slave's syscall entry to the shadow task, which is the in-kernel
   form of the proxy threads in `src/runtime` (`SYSCALL_REQUEST`). Futexes
   work as they do now: executed by the shadow on the master's futex queues.

4. **Preemptive migration.** With tasks in the kernel, a context is moved by
   transferring `pt_regs`, the FPU/XSAVE state (`fpu__save`-style; identical
   `xcr0` on both machines is already checked and reported by the agents) and
   the FS/GS bases, instead of waiting for a migration point.

5. **Memory as NUMA nodes.** Only after 1-4: remote machines can be presented
   to the master's kernel as CPU-less or CPU-ful NUMA nodes backed by the
   pager, so that existing NUMA policy (`mbind`, `set_mempolicy`, balancing)
   applies. This is deliberately **not** faked today: the view series shows
   remote CPUs in `/proc` for people and monitoring tools, but creates no
   NUMA node and no schedulable CPU. `dnumactl topology` shows dnuma's
   logical nodes, and `numactl --hardware` on either machine shows only that
   machine's own nodes.

What stays in userspace: the master's registry, lifecycle, placement policy
and administration (`dnuma-master`, `dnumactl`), and the agents' attach,
lease and fencing logic.

## Related work consulted

Popcorn Linux (distributed thread groups, origin/remote task pairs, page
ownership protocol with per-page owner tracking, futex forwarding to the
origin) is the closest existing design for steps 3-4; its page protocol is
kernel-to-kernel messaging where dnuma uses a single authoritative directory
on the master. CRIU's lazy-pages daemon is the precedent for an external
process serving another process's faults through a passed userfaultfd.
