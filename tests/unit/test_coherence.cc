// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for the page directory / coherence state machine.
//
// The engine is driven against simulated machines (one master and several
// slaves) that implement the node side of the protocol in a few lines each.
// Messages are delivered in random interleavings (FIFO per channel, as the
// real transports guarantee) and the invariants are checked after every
// single delivery. None of this needs hardware; it is where multi-slave
// behaviour is tested beyond what the one physical slave can show.
#include <algorithm>
#include <deque>
#include <map>
#include <random>
#include <set>

#include "master/coherence.h"
#include "utest.h"

using namespace dnuma;

namespace {

struct Copy {
    Access right = Access::None;
    uint64_t value = 0;
    bool staged = false;
    uint64_t staged_value = 0;
    Access requested = Access::None;
};

struct Envelope {
    MsgType type;
    PageMsg msg;
};

struct Sim : CoherenceIo {
    static constexpr ProcId kProc = 1;
    Metrics metrics;
    Coherence coh{*this, metrics};
    std::mt19937 rng;
    int nmachines;
    std::vector<bool> alive;
    std::vector<std::map<PageNo, Copy>> mem;      // per machine
    std::vector<std::deque<Envelope>> down, up;   // master->machine, machine->master
    std::map<PageNo, uint64_t> expected;          // latest value written to each page
    std::vector<std::string> failures;            // coherence_failure() reports
    int violations = 0;
    int fail_parked_grants = 0;                   // inject: next N migration grants fail
    std::set<MachineId> remote_sources;           // machines pages were pulled from
    PageNo first = 0;
    uint64_t npages = 0;

    explicit Sim(int machines, uint64_t pages, unsigned seed = 1)
        : rng(seed), nmachines(machines), alive(size_t(machines), true), mem(size_t(machines)),
          down(size_t(machines)), up(size_t(machines)), npages(pages) {
        coh.create_space(kProc, 1000, 100000);
        if (!coh.allocate(kProc, pages, 0, &first).is_ok()) ++violations;
    }

    // ---- CoherenceIo ----
    void send_page(MachineId to, MsgType type, const PageMsg& m) override { down[to].push_back({type, m}); }
    Incarnation incarnation_of(MachineId m) override { return 100 + m; }
    bool machine_usable(MachineId m) override { return alive[m]; }
    void coherence_failure(ProcId, const std::string& why) override { failures.push_back(why); }

    void violation(const char* what) {
        ++violations;
        printf("    invariant violated: %s\n", what);
    }

    // ---- node behaviour ----
    void request(MachineId m, PageNo page, Access need) {
        Copy& c = mem[m][page];
        if (int(c.right) >= int(need) || int(c.requested) >= int(need)) return;
        c.requested = need;
        PageMsg rq;
        rq.proc = kProc;
        rq.page = page;
        rq.access = need;
        up[m].push_back({MsgType::PageRequest, rq});
    }

    void node_handle(MachineId m, const Envelope& e) {
        const PageMsg& g = e.msg;
        Copy& c = mem[m][g.page];
        PageMsg reply = g;
        switch (e.type) {
        case MsgType::PageGrant: {
            if (g.status != 0) {
                c.requested = Access::None;
                return;
            }
            if ((g.flags & kPageFlagPark) && fail_parked_grants > 0) {
                --fail_parked_grants;
                reply.status = ENOSPC;
                up[m].push_back({MsgType::PageTransferComplete, reply});
                return;
            }
            if (g.source == GrantSource::Zero) {
                c.value = 0;
            } else if (g.source == GrantSource::Local) {
                if (c.right == Access::None) violation("local grant for a page the machine does not hold");
            } else {
                Copy& src = mem[g.src_machine][g.page];
                if (!src.staged) violation("remote grant without a staged copy at the source");
                if (g.src_inc != incarnation_of(g.src_machine)) violation("grant names the wrong source incarnation");
                c.value = src.staged_value;
                remote_sources.insert(g.src_machine);
            }
            c.right = g.access;
            if (int(c.right) >= int(c.requested)) c.requested = Access::None;
            reply.status = 0;
            up[m].push_back({MsgType::PageTransferComplete, reply});
            return;
        }
        case MsgType::PageRevoke: {
            if (c.right == Access::None) {
                violation("revoke for a page the machine does not hold");
                reply.status = ENOENT;
            } else {
                reply.status = 0;
                if (g.mode != RevokeMode::Invalidate) {
                    c.staged = true;
                    c.staged_value = c.value;
                }
                if (g.mode == RevokeMode::DowngradeExport) c.right = Access::Read;
                if (g.mode == RevokeMode::InvalidateExport || g.mode == RevokeMode::Invalidate) c.right = Access::None;
            }
            reply.src_machine = m;
            up[m].push_back({MsgType::PageRevoked, reply});
            return;
        }
        case MsgType::PageRelease:
            c.staged = false;
            return;
        case MsgType::PageAbort:
            if (c.right == Access::None && c.staged) {
                c.value = c.staged_value; // the staged copy is the page again
                c.right = Access::Write;
            } else if (g.flags & kPageFlagRestoreWrite) {
                c.right = Access::Write;
            }
            c.staged = false;
            return;
        default:
            violation("unexpected message type at node");
        }
    }

    void master_handle(MachineId from, const Envelope& e) {
        if (e.type == MsgType::PageRequest) coh.on_request(from, e.msg);
        else if (e.type == MsgType::PageRevoked) coh.on_revoked(from, e.msg);
        else coh.on_complete(from, e.msg);
    }

    // Delivers one pending message, chosen at random. Returns false if idle.
    bool deliver_one() {
        std::vector<std::pair<int, bool>> choices; // (machine, is_down)
        for (int m = 0; m < nmachines; ++m) {
            if (!alive[size_t(m)]) continue; // a dead machine neither receives nor sends
            if (!down[size_t(m)].empty()) choices.emplace_back(m, true);
            if (!up[size_t(m)].empty()) choices.emplace_back(m, false);
        }
        if (choices.empty()) return false;
        auto [m, is_down] = choices[rng() % choices.size()];
        if (is_down) {
            Envelope e = down[size_t(m)].front();
            down[size_t(m)].pop_front();
            node_handle(MachineId(m), e);
        } else {
            Envelope e = up[size_t(m)].front();
            up[size_t(m)].pop_front();
            master_handle(MachineId(m), e);
        }
        check_invariants();
        return true;
    }

    void settle() {
        while (deliver_one()) {}
    }

    // I2 and data coherence, checked from the machines' point of view.
    void check_invariants() {
        for (uint64_t i = 0; i < npages; ++i) {
            PageNo page = first + i;
            int writers = 0, holders = 0;
            for (int m = 0; m < nmachines; ++m) {
                if (!alive[size_t(m)]) continue;
                auto it = mem[size_t(m)].find(page);
                if (it == mem[size_t(m)].end() || it->second.right == Access::None) continue;
                ++holders;
                writers += it->second.right == Access::Write;
                if (it->second.value != expected[page]) violation("a machine can read a stale value");
            }
            if (writers > 1) violation("two machines hold write access to one page");
            if (writers == 1 && holders > 1) violation("a writer coexists with another copy");
        }
    }

    // A machine that holds write access writes the page.
    void local_write(MachineId m, PageNo page) {
        Copy& c = mem[m][page];
        if (c.right != Access::Write) return;
        c.value = ++expected[page];
    }

    // The directory must agree with what the machines actually hold.
    void check_directory_matches() {
        std::map<MachineId, MachineUsage> recomputed;
        for (uint64_t i = 0; i < npages; ++i) {
            PageNo page = first + i;
            const PageEntry* e = coh.lookup(kProc, page);
            if (!e) {
                violation("page vanished from the directory");
                continue;
            }
            if (e->busy) violation("page still busy after settling");
            std::set<MachineId> dir;
            if (e->state == PageState::Exclusive || e->state == PageState::Shared) {
                dir.insert(e->owner);
                ++recomputed[e->owner].owned;
                if (e->readers.empty()) ++recomputed[e->owner].exclusive;
            }
            if (e->state == PageState::Shared)
                for (MachineId r : e->readers) dir.insert(r);
            for (MachineId m : dir) ++recomputed[m].resident;
            for (int m = 0; m < nmachines; ++m) {
                if (!alive[size_t(m)]) continue;
                auto it = mem[size_t(m)].find(page);
                bool holds = it != mem[size_t(m)].end() && it->second.right != Access::None;
                if (holds != (dir.count(MachineId(m)) != 0)) violation("directory and machine disagree about who holds a page");
                if (holds && e->state == PageState::Shared && it->second.right == Access::Write)
                    violation("a machine has write access to a page the directory calls SHARED");
                if (it != mem[size_t(m)].end() && it->second.staged) violation("a staged copy was never released");
            }
        }
        for (int m = 0; m < nmachines; ++m) {
            MachineUsage a = coh.usage(MachineId(m)), b = recomputed[MachineId(m)];
            if (a.resident != b.resident || a.owned != b.owned || a.exclusive != b.exclusive)
                violation("usage counters do not match the directory");
        }
    }
};

} // namespace

TEST(random_interleavings_four_machines) {
    for (unsigned seed = 1; seed <= 12; ++seed) {
        Sim s(4, 6, seed);
        int migrations_done = 0, migrations_failed = 0;
        for (int step = 0; step < 6000; ++step) {
            unsigned r = s.rng() % 100;
            MachineId m = MachineId(s.rng() % 4);
            PageNo page = s.first + s.rng() % s.npages;
            if (r < 30) s.request(m, page, (s.rng() & 1) ? Access::Write : Access::Read);
            else if (r < 45) s.local_write(m, page);
            else if (r < 48)
                s.coh.migrate(Sim::kProc, page, m, [&](Status st) { st.is_ok() ? ++migrations_done : ++migrations_failed; });
            else if (r < 50) s.coh.drop_replica(Sim::kProc, page, m, [](Status) {});
            else if (r == 50) s.fail_parked_grants = 1; // a destination runs out of space
            else s.deliver_one();
        }
        s.settle();
        s.check_invariants();
        s.check_directory_matches();
        CHECK_EQ(s.violations, 0);
        CHECK(s.failures.empty());
        CHECK_EQ(s.coh.active_transactions(), size_t(0));
        CHECK(migrations_done > 0);
        // Pages moved between slaves directly, not only via the master.
        CHECK(s.remote_sources.size() >= 3);
    }
}

TEST(write_waits_for_every_other_copy) {
    Sim s(4, 1);
    PageNo p = s.first;
    s.request(1, p, Access::Write);
    s.settle();
    s.local_write(1, p);
    s.request(2, p, Access::Read);
    s.settle();
    s.request(3, p, Access::Read);
    s.settle();
    const PageEntry* e = s.coh.lookup(Sim::kProc, p);
    CHECK(e->state == PageState::Shared && e->owner == 1 && e->readers.size() == 2);

    // Machine 0 wants to write. Nothing may be granted until machines 1, 2
    // and 3 have all acknowledged giving the page up.
    s.request(0, p, Access::Write);
    while (!s.up[0].empty()) {
        s.master_handle(0, s.up[0].front());
        s.up[0].pop_front();
    }
    CHECK(s.down[0].empty());
    CHECK_EQ(s.down[1].size() + s.down[2].size() + s.down[3].size(), size_t(3));
    for (MachineId m : {MachineId(2), MachineId(3)}) { // two of three acknowledge
        s.node_handle(m, s.down[m].front());
        s.down[m].pop_front();
        s.master_handle(m, s.up[m].front());
        s.up[m].pop_front();
        CHECK(s.down[0].empty()); // still no grant
    }
    s.node_handle(1, s.down[1].front());
    s.down[1].pop_front();
    s.master_handle(1, s.up[1].front());
    s.up[1].pop_front();
    CHECK_EQ(s.down[0].size(), size_t(1)); // now, and only now
    CHECK(s.down[0].front().type == MsgType::PageGrant && s.down[0].front().msg.access == Access::Write);
    CHECK_EQ(s.down[0].front().msg.src_machine, 1u); // pulled straight from the old owner
    s.settle();
    e = s.coh.lookup(Sim::kProc, p);
    CHECK(e->state == PageState::Exclusive && e->owner == 0 && e->readers.empty());
    CHECK_EQ(s.mem[0][p].value, 1u);
    CHECK_EQ(s.violations, 0);
}

TEST(slave_to_slave_transfer_bypasses_master) {
    Sim s(3, 1);
    PageNo p = s.first;
    s.request(1, p, Access::Write);
    s.settle();
    s.local_write(1, p);
    s.request(2, p, Access::Read);
    s.settle();
    CHECK(s.remote_sources.count(1) == 1 && s.remote_sources.size() == 1);
    CHECK(s.mem[0][p].right == Access::None); // the master never held a copy
    CHECK_EQ(s.mem[2][p].value, 1u);
    CHECK_EQ(s.violations, 0);
}

TEST(destination_dies_while_granting) {
    Sim s(3, 1);
    PageNo p = s.first;
    s.request(1, p, Access::Write);
    s.settle();
    s.local_write(1, p);
    Status result;
    bool called = false;
    s.coh.migrate(Sim::kProc, p, 2, [&](Status st) {
        result = st;
        called = true;
    });
    // Revoke reaches the source, which stages the page and acknowledges.
    s.node_handle(1, s.down[1].front());
    s.down[1].pop_front();
    s.master_handle(1, s.up[1].front());
    s.up[1].pop_front();
    CHECK_EQ(s.down[2].size(), size_t(1)); // grant on its way to machine 2
    CHECK(s.mem[1][p].right == Access::None && s.mem[1][p].staged);

    s.alive[2] = false; // ... which now fails
    s.coh.machine_gone(2);
    s.settle();
    CHECK(called && !result.is_ok());
    const PageEntry* e = s.coh.lookup(Sim::kProc, p);
    CHECK(e->state == PageState::Exclusive && e->owner == 1 && !e->busy);
    CHECK(s.mem[1][p].right == Access::Write && !s.mem[1][p].staged);
    CHECK_EQ(s.mem[1][p].value, 1u); // nothing was lost
    CHECK(s.failures.empty());       // and the process is not blamed
    CHECK_EQ(s.coh.inflight_to(2), 0u);
    CHECK_EQ(s.violations, 0);
}

TEST(destination_dies_while_revoking) {
    Sim s(3, 1);
    PageNo p = s.first;
    s.request(1, p, Access::Write);
    s.settle();
    s.local_write(1, p);
    bool called = false, ok = true;
    s.coh.migrate(Sim::kProc, p, 2, [&](Status st) {
        called = true;
        ok = st.is_ok();
    });
    s.alive[2] = false; // dies before the source has even answered
    s.coh.machine_gone(2);
    s.settle();
    CHECK(called && !ok);
    const PageEntry* e = s.coh.lookup(Sim::kProc, p);
    CHECK(e->state == PageState::Exclusive && e->owner == 1);
    CHECK(s.mem[1][p].right == Access::Write && s.mem[1][p].value == 1);
    CHECK(s.failures.empty());
    CHECK_EQ(s.violations, 0);
}

TEST(destination_out_of_space_keeps_source) {
    Sim s(3, 1);
    PageNo p = s.first;
    s.request(0, p, Access::Write);
    s.settle();
    s.local_write(0, p);
    s.fail_parked_grants = 1;
    Status result;
    s.coh.migrate(Sim::kProc, p, 1, [&](Status st) { result = st; });
    s.settle();
    CHECK_EQ(result.code(), ENOSPC);
    const PageEntry* e = s.coh.lookup(Sim::kProc, p);
    CHECK(e->state == PageState::Exclusive && e->owner == 0);
    CHECK(s.mem[0][p].right == Access::Write && s.mem[0][p].value == 1);
    CHECK(s.mem[1][p].right == Access::None);
    CHECK(s.failures.empty());
    // A later attempt succeeds.
    s.coh.migrate(Sim::kProc, p, 1, [&](Status st) { result = st; });
    s.settle();
    CHECK(result.is_ok() && s.coh.lookup(Sim::kProc, p)->owner == 1 && s.mem[1][p].value == 1);
    CHECK_EQ(s.violations, 0);
}

TEST(ownership_handoff_to_existing_replica_moves_no_data) {
    Sim s(3, 1);
    PageNo p = s.first;
    s.request(1, p, Access::Write);
    s.settle();
    s.local_write(1, p);
    s.request(0, p, Access::Read);
    s.settle();
    size_t pulls_before = s.metrics.get("page_transfers");
    bool ok = false;
    s.coh.migrate(Sim::kProc, p, 0, [&](Status st) { ok = st.is_ok(); }); // e.g. draining machine 1
    s.settle();
    CHECK(ok);
    CHECK_EQ(s.metrics.get("page_transfers"), pulls_before); // handoff, not a copy
    const PageEntry* e = s.coh.lookup(Sim::kProc, p);
    CHECK(e->state == PageState::Exclusive && e->owner == 0);
    CHECK(s.mem[1][p].right == Access::None && s.mem[0][p].value == 1);
    CHECK_EQ(s.coh.usage(1).resident, 0u);
    CHECK_EQ(s.violations, 0);
}

TEST(stale_and_duplicate_messages_are_ignored) {
    Sim s(3, 1);
    PageNo p = s.first;
    s.request(1, p, Access::Write);
    s.settle();
    Generation g1 = s.coh.lookup(Sim::kProc, p)->gen;
    s.request(2, p, Access::Write);
    s.settle();
    Generation g2 = s.coh.lookup(Sim::kProc, p)->gen;
    CHECK(g2 > g1);
    // Replays of old-generation acknowledgements must change nothing.
    PageMsg old;
    old.proc = Sim::kProc;
    old.page = p;
    old.gen = g1;
    s.coh.on_revoked(1, old);
    s.coh.on_complete(1, old);
    old.gen = g2 + 5; // and so must ones from the future
    s.coh.on_complete(2, old);
    const PageEntry* e = s.coh.lookup(Sim::kProc, p);
    CHECK(e->state == PageState::Exclusive && e->owner == 2 && e->gen == g2 && !e->busy);
    CHECK_EQ(s.coh.active_transactions(), size_t(0));
}

TEST(allocation_and_free) {
    Sim s(3, 4);
    PageNo a = 0, b = 0;
    CHECK(s.coh.allocate(Sim::kProc, 10, 5, &a).is_ok());
    CHECK(s.coh.allocate(Sim::kProc, 10, 5, &b).is_ok());
    CHECK(b >= a + 10); // regions never overlap
    NodeId home = kNoNode;
    CHECK(s.coh.lookup(Sim::kProc, a + 3, &home) && home == 5);
    CHECK(!s.coh.lookup(Sim::kProc, a + 10 + 100000)); // outside any region

    // Fault a few pages in on different machines, share one, then free.
    s.request(1, a, Access::Write);
    s.request(2, a + 1, Access::Write);
    s.settle();
    s.request(0, a, Access::Read);
    s.settle();
    Status st = Status::err(EIO, "not called");
    s.coh.free_region(Sim::kProc, a, [&](Status r) { st = r; });
    s.settle();
    CHECK(st.is_ok());
    CHECK(!s.coh.lookup(Sim::kProc, a));
    CHECK(s.mem[1][a].right == Access::None && s.mem[0][a].right == Access::None && s.mem[2][a + 1].right == Access::None);
    CHECK_EQ(s.coh.usage(1).resident, 0u);

    // The freed range is reused, and an access to an unallocated address is refused.
    PageNo c = 0;
    CHECK(s.coh.allocate(Sim::kProc, 10, 0, &c).is_ok());
    CHECK_EQ(c, a);
    s.coh.free_region(Sim::kProc, c, [](Status) {});
    s.settle();
    s.request(1, a, Access::Read);
    s.settle();
    CHECK(s.mem[1][a].right == Access::None && s.mem[1][a].requested == Access::None);

    CHECK_EQ(s.coh.allocate(Sim::kProc, 1ull << 40, 0, &c).code(), ENOMEM); // larger than the space
    int code = 0;
    s.coh.free_region(Sim::kProc, a + 1, [&](Status r) { code = r.code(); }); // not the start of a region
    CHECK_EQ(code, EINVAL);
    CHECK_EQ(s.violations, 0);
}

TEST(destroying_a_process_cancels_its_transfers) {
    Sim s(3, 2);
    s.request(1, s.first, Access::Write);
    s.settle();
    int cancelled = 0;
    s.coh.migrate(Sim::kProc, s.first, 2, [&](Status st) { cancelled += st.code() == ECANCELED; });
    CHECK_EQ(s.coh.active_transactions(), size_t(1));
    s.coh.destroy_space(Sim::kProc);
    CHECK_EQ(cancelled, 1);
    CHECK_EQ(s.coh.active_transactions(), size_t(0));
    CHECK_EQ(s.coh.usage(1).resident, 0u);
    CHECK(!s.coh.has_space(Sim::kProc));
    // Late protocol messages for the dead process are harmless.
    PageMsg late;
    late.proc = Sim::kProc;
    late.page = s.first;
    late.gen = 2;
    s.coh.on_revoked(1, late);
    s.coh.on_complete(2, late);
    s.coh.on_request(1, late);
    CHECK(s.failures.empty());
}

TEST(populate_and_rehome) {
    Sim s(3, 8);
    s.coh.mark_populated(Sim::kProc, s.first, 8, 2);
    CHECK_EQ(s.coh.usage(2).owned, 8u);
    CHECK_EQ(s.coh.usage(Sim::kProc, 2).exclusive, 8u);
    int seen = 0;
    s.coh.for_each_page_on(2, [&](ProcId, PageNo, const PageEntry& e) { seen += e.owner == 2; });
    CHECK_EQ(seen, 8);
    s.coh.set_region_home(Sim::kProc, s.first, 9);
    NodeId home = 0;
    s.coh.lookup(Sim::kProc, s.first, &home);
    CHECK_EQ(home, 9u);
    CHECK_EQ(s.coh.allocated_pages(Sim::kProc), 8u);
}

UTEST_MAIN()
