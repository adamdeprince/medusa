/* SPDX-License-Identifier: GPL-2.0-only */
// The authoritative page directory and the page-coherence state machine.
//
// This is the only place where page ownership is decided. It is pure logic:
// all effects go through the CoherenceIo interface, so the same code runs in
// the master (messages to real machines) and in unit tests (simulated
// machines, including several slaves).
//
// Invariants (docs/memory.md):
//   I1. A page has at most one transaction in progress; others queue.
//   I2. Write access is granted to a machine only after every other holder
//       has acknowledged invalidation. Two machines never hold write access
//       to the same page.
//   I3. While a page is in transit, the source's staged copy remains the
//       authoritative data until the destination confirms; an aborted
//       transfer returns ownership to the source without loss.
//   I4. Every committed change increments the page generation, and every
//       message of a transaction carries that generation; anything else is
//       ignored as stale.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/metrics.h"
#include "common/status.h"
#include "common/types.h"
#include "transport/messages.h"

namespace dnuma {

struct PageEntry {
    PageState state = PageState::Unbacked;
    bool busy = false;
    MachineId owner = kNoMachine;   // machine holding the authoritative copy
    Generation gen = 0;
    CtxId last_ctx = 0;             // last fault for which the directory granted access
    MachineId last_machine = kNoMachine;
    std::vector<MachineId> readers; // machines holding read-only replicas (excludes owner)
};

struct Region {
    PageNo first = 0;
    uint64_t count = 0;
    NodeId home = kNoNode; // placement target (logical NUMA domain)
    PageEntry initial; // untouched native bytes belong to the live home mm
    std::map<PageNo, PageEntry> pages; // only pages involved in directory operations
    bool releasing = false;
};

struct MachineUsage {
    uint64_t resident = 0;  // pages with any copy on the machine
    uint64_t owned = 0;     // pages whose authoritative copy is on the machine
    uint64_t exclusive = 0; // owned pages with no replica anywhere else ("dirty")
};

class CoherenceIo {
public:
    virtual ~CoherenceIo() = default;
    virtual void page_handoff(ProcId proc, CtxId writer, MachineId from, CtxId peer, MachineId to, uint64_t cost_ns) {}
    virtual void send_page(MachineId to, MsgType type, const PageMsg& m) = 0;
    virtual Incarnation incarnation_of(MachineId m) = 0;
    // False once a machine is fenced/failed: nothing new may be sent to it.
    virtual bool machine_usable(MachineId m) = 0;
    // The directory can no longer guarantee coherence for this process.
    virtual void coherence_failure(ProcId proc, const std::string& why) = 0;
};

class Coherence {
public:
    using Done = std::function<void(Status)>;

    Coherence(CoherenceIo& io, Metrics& metrics) : io_(io), metrics_(metrics) {}

    // ---- address spaces ----
    void create_space(ProcId proc, PageNo first, uint64_t count);
    // Forgets the process entirely, including transactions in flight.
    void destroy_space(ProcId proc);
    bool has_space(ProcId proc) const { return spaces_.count(proc) != 0; }
    Status allocate(ProcId proc, uint64_t npages, NodeId home, PageNo* first);
    // Enroll an existing mapping at its Linux virtual address. First remote
    // access must fetch its contents from the master, including nonzero data.
    Status register_native_region(ProcId proc, PageNo first, uint64_t count, NodeId home);
    // Invalidates every copy of every page in the region, then removes it.
    void free_region(ProcId proc, PageNo first, Done done);
    // Records pages that a machine materialized as parked zero pages.
    void mark_populated(ProcId proc, PageNo first, uint64_t count, MachineId owner);
    void set_region_home(ProcId proc, PageNo region_first, NodeId home);

    // ---- protocol events ----
    void on_request(MachineId from, const PageMsg& m, CtxId ctx = 0);
    void on_revoked(MachineId from, const PageMsg& m);
    void on_complete(MachineId from, const PageMsg& m);

    // ---- directed operations (drain, eviction, explicit migration) ----
    // Moves the authoritative copy to `dest`, parked. If `dest` already holds
    // a replica, ownership is handed over without copying data.
    void migrate(ProcId proc, PageNo page, MachineId dest, Done done);
    // Removes `holder`'s read-only replica.
    void drop_replica(ProcId proc, PageNo page, MachineId holder, Done done);

    // ---- failure ----
    // Aborts transfers heading to a machine that is gone. Processes that
    // depend on the machine must already have been destroyed.
    void machine_gone(MachineId m);

    // ---- queries ----
    const PageEntry* lookup(ProcId proc, PageNo page, NodeId* home = nullptr) const;
    MachineUsage usage(MachineId m) const;
    MachineUsage usage(ProcId proc, MachineId m) const;
    uint64_t inflight_to(MachineId m) const;
    size_t active_transactions() const { return txns_.size(); }
    size_t transactions_involving(MachineId m) const;
    std::vector<ProcId> processes() const;
    // Calls fn for every page that has a copy on machine m.
    void for_each_page_on(MachineId m,
                          const std::function<void(ProcId, PageNo, const PageEntry&)>& fn) const;
    void for_each_region(ProcId proc, const std::function<void(const Region&)>& fn) const;
    uint64_t allocated_pages(ProcId proc) const;

private:
    struct Key {
        ProcId proc;
        PageNo page;
        bool operator==(const Key& o) const { return proc == o.proc && page == o.page; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return std::hash<uint64_t>()(k.proc * 0x9e3779b97f4a7c15ull ^ k.page); }
    };
    enum class Kind : uint8_t { Fault, Migrate, DropReplica, Discard };
    enum class Phase : uint8_t { Revoking, Granting };

    struct Txn {
        Kind kind{};
        CtxId ctx = 0;
        Phase phase{};
        Generation gen = 0;
        MachineId target = kNoMachine; // requester (fault) / destination (migrate) / holder (drop)
        Access access = Access::None;
        std::set<MachineId> awaiting;  // revocations not yet acknowledged
        MachineId source = kNoMachine; // machine whose staged copy feeds the grant
        RevokeMode source_mode{};
        bool source_staged = false;
        uint32_t slot = 0;
        Incarnation src_inc = 0;
        GrantSource grant_source{};
        bool park = false;
        bool cancelled = false;        // destination vanished while revoking
        Status error;                  // first revocation error
        Done done;
        uint64_t t0 = 0;
        // Target state applied on commit.
        PageState new_state{};
        MachineId new_owner = kNoMachine;
        std::vector<MachineId> new_readers;
        std::deque<std::function<void()>> deferred; // work queued behind this transaction
    };

    struct Space {
        PageNo first = 0;
        uint64_t count = 0;
        std::map<PageNo, Region> regions;
        std::unordered_map<MachineId, MachineUsage> usage;
    };

    Space* space(ProcId proc);
    const Space* space(ProcId proc) const;
    PageEntry* entry(ProcId proc, PageNo page, Region** region = nullptr);
    void set_state(ProcId proc, Space& sp, PageNo page, PageEntry& e, PageState st, MachineId owner,
                   std::vector<MachineId> readers, Generation gen, const char* cause);
    void account(Space& sp, const PageEntry& e, int sign);

    Txn& begin(ProcId proc, PageNo page, PageEntry& e, Kind kind, MachineId target);
    void start_fault(MachineId from, ProcId proc, PageNo page, Access access, CtxId ctx);
    void start_migrate(ProcId proc, PageNo page, MachineId dest, Done done);
    void start_drop(ProcId proc, PageNo page, MachineId holder, Done done);
    void start_discard(ProcId proc, PageNo page, Done done);
    void revoke(ProcId proc, PageNo page, Txn& t, MachineId holder, RevokeMode mode);
    void after_revokes(ProcId proc, PageNo page);
    void grant(ProcId proc, PageNo page, Txn& t);
    void finish(ProcId proc, PageNo page, Status st);
    void defer_or_run(ProcId proc, PageNo page, std::function<void()> fn);

    CoherenceIo& io_;
    Metrics& metrics_;
    std::unordered_map<ProcId, Space> spaces_;
    std::unordered_map<Key, Txn, KeyHash> txns_;
    std::unordered_map<MachineId, MachineUsage> usage_;
    std::unordered_map<MachineId, uint64_t> inflight_;
};

} // namespace dnuma
