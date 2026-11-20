/* SPDX-License-Identifier: GPL-2.0-only */
// The node page pool: the only memory a node ever exposes over RDMA.
//
// It is a single, bounded, page-aligned region split into one sub-pool per
// physical NUMA node (each bound to its node with mbind). Slots hold:
//   * parked pages  - authoritative or read-only copies of distributed pages
//                     that are not currently mapped into an image;
//   * staging copies exported to a peer that is pulling a page;
//   * landing buffers for pages being pulled from a peer.
//
// The pool size is an operator-configured bound (--pool-size): the spec
// requires registered memory to be bounded. Exhaustion is never silent: slot
// allocation fails explicitly and callers either wait for a slot or report
// ENOSPC to the master.
#pragma once

#include <cstdint>
#include <functional>
#include <deque>
#include <vector>

#include "common/status.h"
#include "common/types.h"

namespace dnuma {

class PagePool {
public:
    PagePool() = default;
    ~PagePool();
    PagePool(const PagePool&) = delete;
    PagePool& operator=(const PagePool&) = delete;

    // `phys_nodes` lists the kernel NUMA node numbers of this machine.
    Status init(uint64_t bytes, const std::vector<int>& phys_nodes);

    uint8_t* base() const { return base_; }
    uint64_t bytes() const { return bytes_; }
    uint64_t slots_total() const { return total_; }
    uint64_t slots_used() const { return used_; }

    // Allocates a slot, preferring the sub-pool of `phys_node` (-1 = any).
    // Returns -1 when the pool is exhausted.
    int64_t alloc(int phys_node);
    void free(uint32_t slot);
    uint8_t* addr(uint32_t slot) const { return base_ + uint64_t(slot) * kPageSize; }
    int phys_node_of(uint32_t slot) const;

    // Runs `fn` as soon as a slot is free (immediately if one is free now).
    void when_available(std::function<void()> fn);

private:
    struct Sub {
        int phys_node;
        uint32_t first, count;
        std::vector<uint32_t> free_list;
    };
    uint8_t* base_ = nullptr;
    uint64_t bytes_ = 0, total_ = 0, used_ = 0;
    std::vector<Sub> subs_;
    std::deque<std::function<void()>> waiters_;
    bool draining_waiters_ = false;
};

// Thread memory policy helpers (raw syscalls; libnuma is not required).
// -1 restores the policy the thread had when it first called this.
void set_thread_preferred_node(int phys_node);

// Confines the calling thread, and everything it later creates (threads and
// child processes inherit both settings, also across exec), to the given
// CPUs and to memory of the given NUMA nodes (kernel node numbers).
Status confine_to_nodes(const std::vector<int>& phys_nodes, const std::vector<int>& cpus);

} // namespace dnuma
