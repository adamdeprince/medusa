# Distributed memory

Each logical process has one distributed virtual address range. An address in
it means the same thing in every thread on every machine. The same *values*
on every machine does not mean the same *physical* memory: a CPU can only
touch RAM in its own machine, so a page is physically present on a machine
exactly while that machine holds a copy, and a CPU that touches a page its
machine does not hold takes a fault that the local pager resolves by fetching
the page over RDMA. RDMA is used to move page contents; it is never treated
as CPU-addressable memory, and InfiniBand atomics are not used at all.

Coherence granularity is 4 KiB (the x86-64 base page). Transparent huge pages
are disabled on the range.

## Address space

* Every image maps the same range (default base `0x2000_0000_0000`, size
  1 TiB, both chosen by the master and sent in the registration reply) as
  private anonymous memory with `MAP_NORESERVE | MAP_FIXED_NOREPLACE`, and
  registers all of it with userfaultfd in `MISSING | WP` mode.
* The userfaultfd is created by the image (it must be: a userfaultfd binds to
  the creating `mm`) and passed to the local node daemon, which from then on
  is the only reader. On the master the master hands the image a
  `/dev/userfaultfd` descriptor so that an unprivileged process gets a
  descriptor that can resolve kernel-mode faults; on the slave (5.15, no device
  node) the agent starts the image privileged and it drops to the user's
  uid/gid right after creating the descriptor.
* Allocation is purely a directory operation on the master (first-fit over
  the range). Nothing is mapped or unmapped per allocation. A fault on an
  address inside the range that is not part of an allocation is answered with
  a refusal and the node delivers `SIGSEGV`.
* The last 8 MiB of the range are reserved (never allocated); node daemons use
  them as scratch address space.

## Where a page lives

On the machine that holds it, a page is in exactly one of two places:

* **mapped** - in the local image's anonymous memory, placed on a physical
  NUMA node chosen by the pager (`UFFDIO_COPY` allocates under the calling
  thread's memory policy, so the pager sets `MPOL_PREFERRED` to the page's home
  node before installing it);
* **parked** - in a slot of the node's registered **page pool**, when no local
  image exists to hold it (memory-only use of a machine, or evacuation to a
  machine that runs no thread of the process).

The pool is one bounded region per daemon (`--pool-size`, default 256 MiB),
split into one sub-pool per physical NUMA node (`mbind`). It is the only
memory a node exposes over RDMA. Its slots also serve as staging copies for
pages being exported and as landing buffers for pages being pulled. Parked
capacity is therefore bounded by the pool size; mapped pages are bounded by
the machine's `--capacity`. Exhaustion is explicit: a revocation that needs a
staging slot waits for one, and a transfer that cannot get a landing slot
fails with `ENOSPC` and is aborted without loss.

## The page directory

The master keeps, for every page of every allocation (`src/master/coherence.h`):

```cpp
struct PageEntry {
    PageState state;            // UNBACKED | EXCLUSIVE | SHARED | LOST
    bool busy;                  // a transaction is in progress
    MachineId owner;            // machine holding the authoritative copy
    Generation gen;             // incremented by every committed change
    std::vector<MachineId> readers; // machines holding read-only replicas
};
```

plus, per allocation, the *home node* (the logical NUMA domain it is placed
on). Coherence is tracked per machine; NUMA placement within the machine is
the node pager's job.

### Authoritative states

| State | Meaning |
|---|---|
| `UNBACKED` | Allocated, never touched. No copy exists anywhere; reads as zero. |
| `EXCLUSIVE(o)` | Machine `o` holds the only copy and may write it. ("Dirty": no replica exists.) |
| `SHARED(o, R)` | `o` holds the authoritative copy; `o` and every machine in `R` hold identical read-only copies. |
| `LOST` | The only copy was on a machine that failed. Access kills the process. (Not entered today: see `lifecycle.md`.) |

### Invariants

* **I1** At most one transaction per page; later requests queue behind it.
* **I2** Write access is granted only after *every other holder* has
  acknowledged invalidation. Two machines never hold write access to a page,
  and a writer never coexists with any other copy.
* **I3** While a page is in transit, the source's staged copy remains the
  authoritative data until the destination confirms. An aborted transfer
  returns ownership to the source; nothing is lost.
* **I4** Every committed change increments the generation and every message
  of a transaction carries it; anything else is ignored as stale.

### Transitions

`M` is the requesting machine. "revoke(x, mode)" is `PAGE_REVOKE` to machine
`x`, answered by `PAGE_REVOKED`; "grant" is `PAGE_GRANT`, answered by
`PAGE_TRANSFER_COMPLETE`.

| From | Event | Actions | To |
|---|---|---|---|
| UNBACKED | read or write fault at M | grant(M, write, zero-fill) | EXCLUSIVE(M) |
| EXCLUSIVE(o) | read fault at M != o | revoke(o, downgrade+export); grant(M, read, pull from o); release(o) | SHARED(o, {M}) |
| EXCLUSIVE(o) | write fault at M != o | revoke(o, invalidate+export); grant(M, write, pull from o); release(o) | EXCLUSIVE(M) |
| EXCLUSIVE(o) | fault at o (mapping lost or read-only locally) | grant(o, write, local) | EXCLUSIVE(o) |
| SHARED(o, R) | read fault at M not holding | revoke(o, export); grant(M, read, pull from o); release(o) | SHARED(o, R+M) |
| SHARED(o, R) | write fault at M holding a copy | revoke(every other holder, invalidate); grant(M, write, local) | EXCLUSIVE(M) |
| SHARED(o, R) | write fault at M not holding | revoke(R, invalidate) and revoke(o, invalidate+export); grant(M, write, pull from o); release(o) | EXCLUSIVE(M) |
| EXCLUSIVE/SHARED | migrate to D (drain, eviction, API) | as write fault, but the page is not forced into use; if D already holds a replica, ownership is *handed over* with no data transfer | EXCLUSIVE(D) |
| SHARED(o, R) | drop replica at r | revoke(r, invalidate) | SHARED(o, R-r) or EXCLUSIVE(o) |
| any backed | free | revoke(all holders, invalidate) | UNBACKED, region removed |
| in transit | destination reports failure or disappears | abort(source): staged copy re-adopted | EXCLUSIVE(source) (or unchanged for non-invalidating exports) |
| any | a holder cannot revoke | transaction fails; the process is terminated (its memory is in doubt) | - |

A transaction that fails for a *fault* (the faulting thread cannot be given
the page) terminates the process; one that fails for a *migration* is reported
to the caller and the page stays where it was.

### Node-side state

Each node keeps, per page it has anything to do with: the access right the
master granted this machine (none/read/write), where the copy is (absent /
mapped / parked), whether the local mapping is currently writable, any staging
slot held for a peer, and the highest access it has asked the master for.
The node acts on faults as follows (`NodeCore::on_fault`): already satisfiable
-> wake; machine owns the page but the local mapping is read-only -> unprotect
locally; parked with sufficient right -> install from the pool; otherwise send
`PAGE_REQUEST` (once per needed level) and leave the thread blocked.

## Revoking access: write-protect is not invalidate

There are two different things to take away, and they need different
mechanisms.

**Stopping writes** (downgrade to shared, or the first half of an
invalidation). The pager issues `UFFDIO_WRITEPROTECT` on the image's
userfaultfd. This works from outside the process, the kernel changes the PTE
and flushes the TLB of every CPU running the address space, and from then on a
write blocks in a userfaultfd write-protect fault. The pager then copies the
(now stable) contents with `process_vm_readv` into a pool slot.

**Removing a readable mapping.** A write-protected page can still be read, so
a machine that must give up a page entirely (another machine is about to write
it) must lose the mapping itself, or its threads would keep reading stale
data. This is the step the brief singles out. What dnuma does:

1. write-protect (above) and, if the contents are needed, copy them out;
2. have the mapping removed with `MADV_DONTNEED` on the page - by the pager
   itself through `process_madvise()` where the kernel allows it, otherwise by
   the image's runtime service thread on request. `MADV_DONTNEED` clears the
   PTE and performs the TLB shootdown on all CPUs before returning;
3. **verify** through `/proc/<pid>/pagemap` that the page is neither present
   nor swapped;
4. only then wake any thread blocked on the page (it re-faults as "missing")
   and acknowledge the revocation to the master, which only then grants write
   access elsewhere.

If the unmap is not confirmed within a deadline, or verification fails, the
revocation is *not* acknowledged and the image is killed. So safety never
depends on the image's cooperation: no machine is granted write access while
a stale mapping can exist. Cooperation affects liveness only, and the
kill-on-timeout bounds that.

`tests/integration/progs/t_coherence.cc` demonstrates it: readers on both
machines continuously read a pair of pages while a writer on the other machine
updates them; a single stale read would violate the checked ordering. The node
metrics `revoke_verified` / `revoke_verify_failures` count the verifications.

### What the kernel permits (checked in the Linux 7.3.5 source)

dnuma is built against the Linux 7.3.5 UAPI headers and detects features at
run time. Reading 7.3.5's `mm/userfaultfd.c` and `mm/madvise.c` established:

| Facility | Finding | Consequence |
|---|---|---|
| `UFFDIO_WRITEPROTECT` | Works from any holder of the descriptor. | Used by the pager to stop writers. |
| `UFFDIO_RWPROTECT` / `UFFD_FEATURE_RWP` (new in 7.3) | Makes present pages inaccessible and reports accesses; works from outside the process. But `UFFDIO_REGISTER_MODE_WP` and `MODE_RWP` **cannot be registered on the same VMA** (they share the uffd PTE bit; registration returns `EINVAL`). GUP always faults on an RWP page. | A protocol that needs read-only sharing (write-protect) cannot also use RWP for invalidation. Not used. |
| `UFFDIO_MOVE` | Atomically removes a page from a mapping with a TLB flush - but `userfaultfd_move()` rejects callers whose `mm` is not the target's ("Do not allow cross-mm moves"). | Cannot be used by an external pager. An in-process caller gains nothing over `MADV_DONTNEED`. Not used. |
| `process_madvise()` | Remote advice is limited to `MADV_COLD`, `MADV_PAGEOUT`, `MADV_WILLNEED`, `MADV_COLLAPSE`. | A pager cannot unmap in another process on stock kernels. `kernel/` carries a small patch that permits remote `MADV_DONTNEED` for ptrace-capable callers; the pager probes for it at image start and uses it when present. |
| `/dev/userfaultfd` (6.1+) | Lets a privileged daemon delegate creation of a kernel-fault-capable descriptor. | Used on the master; the slave's 5.15 lacks it. |
| `UFFDIO_POISON` (6.6+) | Marks pages so that access raises `SIGBUS`. | Natural implementation of `LOST`; not needed while dependents of a failed machine are killed. |

On both of today's kernels the cooperative-plus-verified path is therefore the
one in use (`revocation: cooperative unmap + pagemap verification` in the
daemon logs).

## Physical placement

"Allocate on node N" records N as the allocation's home. A page is
materialized where it is first touched (the toucher must hold it to access
it); when that is the home machine it is placed on the home physical node.
`DNUMA_ALLOC_POPULATE` materializes zero pages on the home machine
immediately, parked in its pool. `dnuma_migrate_pages()` moves pages to a
machine explicitly (eviction to remote memory, or pulling them home).
`t_memory` verifies placement by asking the slave's own kernel
(`move_pages(2)` query) which physical node backs the pages.

Not implemented: automatic eviction or rebalancing policy. Capacity
(`--capacity`) limits directed placement (drain, migration, populate); a fault
is always served, since refusing it would stall the thread.

## Tracing and metrics

`--trace-pages` logs every directory transition (process, page address, old
state and owner, new state, owner and readers, generation, cause) on the
master, and every grant/revoke with the local state on each node. Counters and
latency histograms (log2 buckets with four sub-buckets, so distributions and
not just means) are listed by `dnumactl metrics`: fault latency per node, RDMA
read latency, revocation latency, transaction latency, counts of remote
faults, transfers, invalidations, ownership transitions, migrations.

## Memory overhead

A directory entry is 48 bytes per allocated page (about 1.2% of the memory it
describes) plus a node-side record of similar size per page a node holds.
This is adequate for the current scale and would need a denser encoding
(extent-based for uniform regions) for very large address spaces.
