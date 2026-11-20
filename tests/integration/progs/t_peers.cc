// SPDX-License-Identifier: GPL-2.0-only
// Two slaves: data written on one slave is read and then rewritten on the
// other. The page contents travel directly between the two slaves' RDMA
// endpoints; the master only coordinates.
#include "check.h"

static const uint64_t kWords = 256 * 4096 / 8;

struct Ctl {
    uint64_t* data;
    dnuma_location where[3];
    uint64_t bad[3];
};

static uint64_t fill(void* p) {
    Ctl* c = static_cast<Ctl*>(p);
    dnuma_where(&c->where[0]);
    for (uint64_t i = 0; i < kWords; ++i) c->data[i] = i * 31 + 5;
    return 0;
}
static uint64_t check_and_rewrite(void* p) {
    Ctl* c = static_cast<Ctl*>(p);
    dnuma_where(&c->where[1]);
    uint64_t bad = 0;
    for (uint64_t i = 0; i < kWords; ++i) bad += c->data[i] != i * 31 + 5;
    c->bad[1] = bad;
    for (uint64_t i = 0; i < kWords; ++i) c->data[i] = i * 77 + 9;
    return bad;
}
static uint64_t check_again(void* p) {
    Ctl* c = static_cast<Ctl*>(p);
    dnuma_where(&c->where[2]);
    uint64_t bad = 0;
    for (uint64_t i = 0; i < kWords; ++i) bad += c->data[i] != i * 77 + 9;
    c->bad[2] = bad;
    return bad;
}

int main() {
    REQUIRE(dnuma_active(), "must be started with dnuma-run");
    Topo t = load_topology();
    int a = -1, b = -1;
    for (int i = 0; i < t.count; ++i) {
        const dnuma_node_info& n = t.nodes[i];
        if (n.is_master || !n.present) continue;
        if (a < 0) a = i;
        else if (n.machine != t.nodes[a].machine && b < 0) b = i;
    }
    REQUIRE(a >= 0 && b >= 0, "this test needs two attached slaves");
    const dnuma_node_info &na = t.nodes[a], &nb = t.nodes[b];
    printf("slave A: node %d on %s (machine %d); slave B: node %d on %s (machine %d)\n", na.logical, na.host, na.machine,
           nb.logical, nb.host, nb.machine);

    Ctl* c = static_cast<Ctl*>(dnuma_alloc(sizeof(Ctl), DNUMA_NODE_ANY, 0));
    c->data = static_cast<uint64_t*>(dnuma_alloc(kWords * 8, na.logical, 0));
    REQUIRE(c && c->data, "alloc");
    dnuma_thread_t th;
    dnuma_page_info pi;

    REQUIRE(dnuma_thread_create(&th, na.logical, fill, c) == 0, "create: %s", dnuma_last_error());
    REQUIRE(dnuma_thread_join(th, nullptr, nullptr) == 0, "join");
    dnuma_query_page(&c->data[kWords / 2], &pi);
    CHECK(pi.state == DNUMA_PAGE_EXCLUSIVE && pi.owner_machine == na.machine, "after A wrote: %s owner machine %d", state_name(pi.state), pi.owner_machine);

    REQUIRE(dnuma_thread_create(&th, nb.logical, check_and_rewrite, c) == 0, "create: %s", dnuma_last_error());
    REQUIRE(dnuma_thread_join(th, nullptr, nullptr) == 0, "join");
    CHECK(c->bad[1] == 0, "B saw %" PRIu64 " wrong words", c->bad[1]);
    dnuma_query_page(&c->data[kWords / 2], &pi);
    CHECK(pi.state == DNUMA_PAGE_EXCLUSIVE && pi.owner_machine == nb.machine, "after B rewrote: %s owner machine %d", state_name(pi.state), pi.owner_machine);

    REQUIRE(dnuma_thread_create(&th, na.logical, check_again, c) == 0, "create: %s", dnuma_last_error());
    REQUIRE(dnuma_thread_join(th, nullptr, nullptr) == 0, "join");
    CHECK(c->bad[2] == 0, "A saw %" PRIu64 " wrong words after B rewrote", c->bad[2]);
    dnuma_query_page(&c->data[kWords / 2], &pi);
    CHECK(pi.state == DNUMA_PAGE_SHARED && pi.owner_machine == nb.machine && pi.nreaders == 1, "after A reread: %s owner machine %d", state_name(pi.state), pi.owner_machine);
    printf("ran on: %s -> %s -> %s\n", c->where[0].host, c->where[1].host, c->where[2].host);
    CHECK(c->where[0].machine == na.machine && c->where[1].machine == nb.machine && c->where[2].machine == na.machine, "wrong machines");
    return finish("peers");
}
