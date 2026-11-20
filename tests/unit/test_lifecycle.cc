// SPDX-License-Identifier: GPL-2.0-only
#include "master/lifecycle.h"
#include "master/topology.h"
#include "utest.h"

using namespace dnuma;
using S = SlaveState;
using E = LifecycleEvent;

static bool go(S from, E ev, S expect, bool pending = false) {
    S to;
    return lifecycle_next(from, ev, pending, &to) && to == expect;
}
static bool illegal(S from, E ev) {
    S to = from;
    return !lifecycle_next(from, ev, false, &to) && to == from;
}

TEST(documented_transitions) {
    CHECK(go(S::Disconnected, E::Connect, S::Connecting));
    CHECK(go(S::Connecting, E::ConnectOk, S::Active));
    CHECK(go(S::Connecting, E::ConnectFail, S::Disconnected));
    CHECK(go(S::Active, E::Drain, S::Draining));
    CHECK(go(S::Draining, E::DrainDone, S::Disconnected));
    CHECK(go(S::Draining, E::DrainCancel, S::Active));
    CHECK(go(S::Active, E::HardDisconnect, S::Fencing));
    CHECK(go(S::Draining, E::HardDisconnect, S::Fencing));
    CHECK(go(S::Active, E::Failure, S::Failed));
    CHECK(go(S::Draining, E::Failure, S::Failed));
    CHECK(go(S::Failed, E::FenceBegin, S::Fencing));
    CHECK(go(S::Fencing, E::FenceConfirmed, S::Disconnected));
    CHECK(go(S::Fencing, E::FenceUnconfirmed, S::FenceIncomplete));
    CHECK(go(S::FenceIncomplete, E::LeaseExpired, S::Disconnected));
    CHECK(go(S::FenceIncomplete, E::Connect, S::Connecting));
}

TEST(failed_drain_never_becomes_a_hard_disconnect) {
    // The only exits from DRAINING are done, cancel, an explicit hard
    // disconnect, or a detected failure. There is no implicit path to FENCING.
    for (E ev : {E::Connect, E::ConnectOk, E::ConnectFail, E::Drain, E::FenceBegin, E::FenceConfirmed, E::FenceUnconfirmed, E::LeaseExpired})
        CHECK(illegal(S::Draining, ev));
    CHECK(go(S::Draining, E::DrainCancel, S::Active));
}

TEST(unproven_fence_is_sticky) {
    // A reconnect attempt that fails while an old incarnation is unproven
    // must return to FENCE_INCOMPLETE, never to DISCONNECTED.
    CHECK(go(S::Connecting, E::ConnectFail, S::FenceIncomplete, true));
    // Resources of an unfenced slave cannot be made ACTIVE by any shortcut.
    for (E ev : {E::ConnectOk, E::Drain, E::DrainDone, E::DrainCancel, E::HardDisconnect, E::Failure, E::FenceBegin})
        CHECK(illegal(S::FenceIncomplete, ev));
}

TEST(illegal_transitions_rejected) {
    CHECK(illegal(S::Disconnected, E::Drain));
    CHECK(illegal(S::Disconnected, E::HardDisconnect));
    CHECK(illegal(S::Disconnected, E::ConnectOk));
    CHECK(illegal(S::Active, E::Connect));
    CHECK(illegal(S::Active, E::ConnectOk));
    CHECK(illegal(S::Active, E::FenceConfirmed));
    CHECK(illegal(S::Fencing, E::Connect));
    CHECK(illegal(S::Fencing, E::Drain));
    CHECK(illegal(S::Failed, E::Connect));
    CHECK(illegal(S::Failed, E::ConnectOk));
}

TEST(placement_and_service_predicates) {
    CHECK(lifecycle_placeable(S::Active));
    for (S s : {S::Disconnected, S::Connecting, S::Draining, S::Failed, S::Fencing, S::FenceIncomplete}) CHECK(!lifecycle_placeable(s));
    CHECK(lifecycle_serving(S::Active) && lifecycle_serving(S::Draining));
    for (S s : {S::Disconnected, S::Connecting, S::Failed, S::Fencing, S::FenceIncomplete}) CHECK(!lifecycle_serving(s));
}

static MachineResources machine(int nodes) {
    MachineResources r;
    for (int i = 0; i < nodes; ++i) {
        NumaNodeInfo n;
        n.phys_index = i;
        n.cpus = {i, i + 4};
        n.mem_total = 1ull << 30;
        r.nodes.push_back(n);
    }
    return r;
}

TEST(topology_identities_are_stable) {
    Topology t;
    auto m = t.attach(0, "master", machine(4));
    CHECK_EQ(m.size(), size_t(4));
    CHECK_EQ(m[3].logical, 3);
    auto a = t.attach(1, "uuid-a", machine(4));
    auto b = t.attach(2, "uuid-b", machine(2));
    CHECK_EQ(a[0].logical, 4);
    CHECK_EQ(b[0].logical, 8);
    CHECK_EQ(t.present_count(), size_t(10));

    // Detach A: its numbers are retained, nobody else is renumbered.
    t.detach(1);
    CHECK_EQ(t.present_count(), size_t(6));
    CHECK(t.by_logical(5) && !t.by_logical(5)->present);
    CHECK(t.by_logical(8) && t.by_logical(8)->present && t.by_logical(8)->machine == 2);

    // A third machine attaching meanwhile gets new numbers, not A's.
    auto c = t.attach(3, "uuid-c", machine(4));
    CHECK_EQ(c[0].logical, 10);

    // A comes back (same UUID): same node ids and logical numbers as before.
    auto a2 = t.attach(1, "uuid-a", machine(4));
    for (size_t i = 0; i < 4; ++i) {
        CHECK_EQ(a2[i].node, a[i].node);
        CHECK_EQ(a2[i].logical, a[i].logical);
    }
    CHECK(t.by_logical(5)->present);
    CHECK_EQ(t.nodes_of(1).size(), size_t(4));
    CHECK_EQ(t.all().size(), size_t(14));
}

TEST(topology_tracks_resource_changes) {
    Topology t;
    t.attach(1, "uuid-a", machine(2));
    t.detach(1);
    // The machine comes back with a different shape (e.g. a DIMM was pulled
    // or a node hot-added): existing nodes keep their ids, new ones get new ids.
    auto again = t.attach(1, "uuid-a", machine(3));
    CHECK_EQ(again[0].logical, 0);
    CHECK_EQ(again[1].logical, 1);
    CHECK_EQ(again[2].logical, 2);
    t.update_free(1, {111, 222, 333});
    CHECK_EQ(t.by_logical(1)->mem_free, 222u);
}

UTEST_MAIN()
