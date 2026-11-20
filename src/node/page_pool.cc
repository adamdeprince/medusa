// SPDX-License-Identifier: GPL-2.0-only
#include "node/page_pool.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <linux/mempolicy.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common/log.h"

namespace dnuma {

namespace {
// Builds a node mask wide enough for `node` (kernel ABI: array of unsigned long).
std::vector<unsigned long> node_mask(int node, unsigned long* maxnode) {
    size_t bits_per = sizeof(unsigned long) * 8;
    std::vector<unsigned long> mask(size_t(node) / bits_per + 1, 0);
    mask[size_t(node) / bits_per] |= 1ul << (size_t(node) % bits_per);
    *maxnode = mask.size() * bits_per + 1;
    return mask;
}

// A thread's memory policy as the kernel reports it.
struct Policy {
    int mode = MPOL_DEFAULT;
    std::vector<unsigned long> mask;
    bool empty() const {
        for (unsigned long w : mask)
            if (w) return false;
        return true;
    }
};

// The calling thread's current policy. If it cannot be read, the default
// policy is assumed.
Policy current_policy() {
    const size_t bits_per = sizeof(unsigned long) * 8;
    // The kernel refuses a mask shorter than its own node count (EINVAL) and
    // does not say how long it must be, so grow until it is accepted. No
    // kernel supports more than 2^10 nodes (CONFIG_NODES_SHIFT); the upper
    // bound only stops the loop if EINVAL has some other cause.
    for (size_t words = 16; words <= 1024; words *= 2) {
        Policy p;
        p.mask.assign(words, 0);
        if (syscall(SYS_get_mempolicy, &p.mode, p.mask.data(), words * bits_per, nullptr, 0) == 0) return p;
        if (errno != EINVAL) break;
    }
    return Policy{};
}

long apply_policy(const Policy& p) {
    if (p.empty()) return syscall(SYS_set_mempolicy, p.mode, nullptr, 0);
    return syscall(SYS_set_mempolicy, p.mode, p.mask.data(), p.mask.size() * sizeof(unsigned long) * 8 + 1);
}
} // namespace

Status confine_to_nodes(const std::vector<int>& phys_nodes, const std::vector<int>& cpus) {
    if (phys_nodes.empty() || cpus.empty()) return Status::err(EINVAL, "nothing to confine to");
    int max_cpu = 0;
    for (int c : cpus) max_cpu = std::max(max_cpu, c);
    size_t sz = CPU_ALLOC_SIZE(size_t(max_cpu) + 1);
    cpu_set_t* set = CPU_ALLOC(size_t(max_cpu) + 1);
    if (!set) return Status::err(ENOMEM, "CPU set");
    CPU_ZERO_S(sz, set);
    for (int c : cpus) CPU_SET_S(size_t(c), sz, set);
    int rc = sched_setaffinity(0, sz, set);
    int err = errno;
    CPU_FREE(set);
    if (rc != 0) return Status::err(err, "sched_setaffinity to the offered CPUs: %s", strerror(err));

    Policy p;
    p.mode = MPOL_BIND;
    const size_t bits_per = sizeof(unsigned long) * 8;
    for (int n : phys_nodes) {
        if (p.mask.size() <= size_t(n) / bits_per) p.mask.resize(size_t(n) / bits_per + 1, 0);
        p.mask[size_t(n) / bits_per] |= 1ul << (size_t(n) % bits_per);
    }
    if (apply_policy(p) != 0) return Status::sys("set_mempolicy(MPOL_BIND) to the offered NUMA nodes");
    return Status::ok();
}

void set_thread_preferred_node(int phys_node) {
    // Whatever policy the thread started with (the default, or a binding the
    // process was confined to) is what "no preference" goes back to.
    static thread_local Policy inherited = current_policy();
    static thread_local int current = -2;
    if (current == phys_node) return;
    long rc;
    if (phys_node < 0) {
        rc = apply_policy(inherited);
    } else if ((inherited.mode & ~MPOL_MODE_FLAGS) == MPOL_BIND) {
        // Confined to a node set: the preferred node must stay within it, and
        // a binding to the single node expresses exactly that.
        Policy p;
        p.mode = MPOL_BIND;
        unsigned long maxnode;
        p.mask = node_mask(phys_node, &maxnode);
        bool inside = p.mask.size() <= inherited.mask.size() &&
                      (inherited.mask[p.mask.size() - 1] & p.mask.back()) != 0;
        rc = inside ? apply_policy(p) : apply_policy(inherited);
    } else {
        unsigned long maxnode;
        std::vector<unsigned long> mask = node_mask(phys_node, &maxnode);
        rc = syscall(SYS_set_mempolicy, MPOL_PREFERRED, mask.data(), maxnode);
    }
    if (rc != 0) LOG_WARN("set_mempolicy(node %d) failed: %s", phys_node, strerror(errno));
    current = phys_node;
}

PagePool::~PagePool() {
    if (base_) munmap(base_, bytes_);
}

Status PagePool::init(uint64_t bytes, const std::vector<int>& phys_nodes) {
    if (phys_nodes.empty()) return Status::err(EINVAL, "page pool needs at least one NUMA node");
    uint64_t slots = bytes / kPageSize;
    if (slots < phys_nodes.size() * 16) return Status::err(EINVAL, "page pool too small (%llu bytes)", (unsigned long long)bytes);
    if (slots > UINT32_MAX) // slot numbers are 32-bit on the wire
        return Status::err(EINVAL, "page pool too large for 32-bit slot numbers");
    bytes_ = slots * kPageSize;
    void* p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return Status::sys("mmap page pool (%llu bytes)", (unsigned long long)bytes_);
    base_ = static_cast<uint8_t*>(p);
    // Registered memory must never be duplicated into a forked child, and
    // huge pages would defeat per-node binding of individual slots.
    madvise(base_, bytes_, MADV_DONTFORK);
    madvise(base_, bytes_, MADV_NOHUGEPAGE);
    total_ = slots;

    uint64_t per = slots / phys_nodes.size();
    uint32_t first = 0;
    for (size_t i = 0; i < phys_nodes.size(); ++i) {
        Sub s;
        s.phys_node = phys_nodes[i];
        s.first = first;
        s.count = uint32_t(i + 1 == phys_nodes.size() ? slots - first : per);
        unsigned long maxnode;
        std::vector<unsigned long> mask = node_mask(s.phys_node, &maxnode);
        if (syscall(SYS_mbind, base_ + uint64_t(s.first) * kPageSize, uint64_t(s.count) * kPageSize,
                    MPOL_BIND, mask.data(), maxnode, 0) != 0)
            LOG_WARN("page pool: mbind to node %d failed: %s (slots stay unbound)", s.phys_node,
                     strerror(errno));
        s.free_list.reserve(s.count);
        for (uint32_t k = s.count; k > 0; --k) s.free_list.push_back(s.first + k - 1);
        first += s.count;
        subs_.push_back(std::move(s));
    }
    return Status::ok();
}

int64_t PagePool::alloc(int phys_node) {
    Sub* pick = nullptr;
    for (Sub& s : subs_)
        if (s.phys_node == phys_node && !s.free_list.empty()) pick = &s;
    if (!pick) {
        // Preferred sub-pool is empty (or no preference): take from the fullest.
        for (Sub& s : subs_)
            if (!s.free_list.empty() && (!pick || s.free_list.size() > pick->free_list.size())) pick = &s;
    }
    if (!pick) return -1;
    uint32_t slot = pick->free_list.back();
    pick->free_list.pop_back();
    ++used_;
    return slot;
}

int PagePool::phys_node_of(uint32_t slot) const {
    for (const Sub& s : subs_)
        if (slot >= s.first && slot < s.first + s.count) return s.phys_node;
    return -1;
}

void PagePool::free(uint32_t slot) {
    for (Sub& s : subs_) {
        if (slot >= s.first && slot < s.first + s.count) {
            s.free_list.push_back(slot);
            --used_;
            break;
        }
    }
    if (draining_waiters_) return;
    draining_waiters_ = true;
    while (!waiters_.empty() && used_ < total_) {
        auto fn = std::move(waiters_.front());
        waiters_.pop_front();
        fn();
    }
    draining_waiters_ = false;
}

void PagePool::when_available(std::function<void()> fn) {
    if (used_ < total_ && waiters_.empty()) fn();
    else waiters_.push_back(std::move(fn));
}

} // namespace dnuma
