// SPDX-License-Identifier: GPL-2.0-only
// Distributed memory: physical placement, contents, ownership transfer,
// read sharing, invalidation, populate, explicit migration.
#include <sys/syscall.h>
#include <unistd.h>

#include "check.h"

static const uint64_t kPages = 512;
static const uint64_t kWords = kPages * 4096 / 8;

struct Ctl {
    dnuma_location where;
    int phys_node_of_page[8]; // physical NUMA node backing sampled pages, as seen where they were written
    uint64_t readback_bad;
    uint64_t* data;
};

static uint64_t pattern(uint64_t i, uint64_t salt) { return (i + 1) * 0x9e3779b97f4a7c15ull ^ salt; }

// Runs on the slave: writes the whole buffer, then asks its own kernel which
// physical NUMA node each sampled page landed on.
static uint64_t remote_fill(void* p) {
    Ctl* c = static_cast<Ctl*>(p);
    dnuma_where(&c->where);
    for (uint64_t i = 0; i < kWords; ++i) c->data[i] = pattern(i, 1);
    for (int k = 0; k < 8; ++k) {
        void* page = reinterpret_cast<char*>(c->data) + uint64_t(k) * (kPages / 8) * 4096;
        int status = -1;
        // move_pages with a NULL node list only queries placement.
        long rc = syscall(SYS_move_pages, 0, 1ul, &page, nullptr, &status, 0);
        c->phys_node_of_page[k] = rc == 0 ? status : -1000;
    }
    return 1;
}

// Runs on the slave after the master rewrote the buffer: verifies it.
static uint64_t remote_verify(void* p) {
    Ctl* c = static_cast<Ctl*>(p);
    uint64_t bad = 0;
    for (uint64_t i = 0; i < kWords; ++i) bad += c->data[i] != pattern(i, 2);
    c->readback_bad = bad;
    return bad;
}

int main() {
    REQUIRE(dnuma_active(), "must be started with dnuma-run");
    Topo t = load_topology();
    REQUIRE(t.first_slave >= 0, "no slave attached");
    // Use the slave's second NUMA domain when it has one, to show placement
    // is per domain and not merely per machine.
    int node = t.first_slave;
    const dnuma_node_info* base = node_by_logical(t, node);
    if (const dnuma_node_info* n1 = node_by_logical(t, node + 1))
        if (n1->machine == base->machine && n1->present) node = n1->logical;
    const dnuma_node_info* ni = node_by_logical(t, node);
    printf("target: logical node %d = %s physical node %d\n", node, ni->host, ni->phys_index);

    Ctl* c = static_cast<Ctl*>(dnuma_alloc(sizeof(Ctl), node, 0));
    uint64_t* data = static_cast<uint64_t*>(dnuma_alloc(kPages * 4096, node, 0));
    REQUIRE(c && data, "alloc: %s", dnuma_last_error());
    c->data = data;

    dnuma_page_info pi;
    REQUIRE(dnuma_query_page(&data[0], &pi) == 0, "query: %s", dnuma_last_error());
    CHECK(pi.state == DNUMA_PAGE_UNBACKED, "fresh page is %s", state_name(pi.state));
    CHECK(pi.home_node == node, "home node %d", pi.home_node);

    // --- write on the slave ---
    dnuma_thread_t th;
    uint64_t res = 0;
    dnuma_thread_info info;
    REQUIRE(dnuma_thread_create(&th, node, remote_fill, c) == 0, "create: %s", dnuma_last_error());
    REQUIRE(dnuma_thread_join(th, &res, &info) == 0, "join: %s", dnuma_last_error());
    REQUIRE(dnuma_query_page(&data[kWords / 2], &pi) == 0, "query");
    CHECK(pi.state == DNUMA_PAGE_EXCLUSIVE && strcmp(pi.owner_host, ni->host) == 0,
          "after remote write: %s owner %s", state_name(pi.state), pi.owner_host);
    uint64_t gen_after_write = pi.generation;

    // --- physical backing: the slave's kernel reports the NUMA node ---
    CHECK(strcmp(c->where.host, ni->host) == 0, "writer ran on %s", c->where.host);
    for (int k = 0; k < 8; ++k)
        CHECK(c->phys_node_of_page[k] == ni->phys_index, "sample page %d is on physical node %d of %s, expected %d", k,
              c->phys_node_of_page[k], c->where.host, ni->phys_index);
    printf("pages written on %s are physically on its NUMA node %d (kernel-reported)\n", c->where.host, c->phys_node_of_page[0]);

    // --- read on the master: contents arrive, both sides share read-only ---
    uint64_t bad = 0;
    for (uint64_t i = 0; i < kWords; ++i) bad += data[i] != pattern(i, 1);
    CHECK(bad == 0, "%" PRIu64 " words differ when read on the master", bad);
    REQUIRE(dnuma_query_page(&data[kWords / 2], &pi) == 0, "query");
    CHECK(pi.state == DNUMA_PAGE_SHARED && strcmp(pi.owner_host, ni->host) == 0 && pi.nreaders == 1,
          "after master read: %s owner %s readers %d", state_name(pi.state), pi.owner_host, pi.nreaders);
    CHECK(pi.generation > gen_after_write, "generation did not advance (%" PRIu64 ")", pi.generation);

    // --- write on the master: the slave's copy must be invalidated ---
    for (uint64_t i = 0; i < kWords; ++i) data[i] = pattern(i, 2);
    REQUIRE(dnuma_query_page(&data[kWords / 2], &pi) == 0, "query");
    CHECK(pi.state == DNUMA_PAGE_EXCLUSIVE && strcmp(pi.owner_host, t.master_host) == 0 && pi.nreaders == 0,
          "after master write: %s owner %s readers %d", state_name(pi.state), pi.owner_host, pi.nreaders);

    // --- the slave reads what the master wrote ---
    c->readback_bad = ~0ull;
    REQUIRE(dnuma_thread_create(&th, node, remote_verify, c) == 0, "create: %s", dnuma_last_error());
    REQUIRE(dnuma_thread_join(th, &res, nullptr) == 0, "join");
    CHECK(res == 0 && c->readback_bad == 0, "slave saw %" PRIu64 " stale/incorrect words", c->readback_bad);
    REQUIRE(dnuma_query_page(&data[kWords / 2], &pi) == 0, "query");
    CHECK(pi.state == DNUMA_PAGE_SHARED && strcmp(pi.owner_host, t.master_host) == 0,
          "after slave read: %s owner %s", state_name(pi.state), pi.owner_host);

    // --- explicit migration: push the pages back to the slave (eviction) ---
    size_t moved = 0;
    int rc = dnuma_migrate_pages(data, kPages * 4096, node, &moved);
    CHECK(rc == 0, "migrate: %s", dnuma_last_error());
    REQUIRE(dnuma_query_page(&data[kWords / 2], &pi) == 0, "query");
    CHECK(pi.state == DNUMA_PAGE_EXCLUSIVE && strcmp(pi.owner_host, ni->host) == 0,
          "after migration: %s owner %s", state_name(pi.state), pi.owner_host);
    printf("migrated %zu pages to %s\n", moved, ni->host);
    bad = 0;
    for (uint64_t i = 0; i < kWords; ++i) bad += data[i] != pattern(i, 2);
    CHECK(bad == 0, "%" PRIu64 " words lost by migration", bad);

    // --- populate: physically backed on the slave before anyone touches it ---
    uint64_t* pop = static_cast<uint64_t*>(dnuma_alloc(64 * 4096, node, DNUMA_ALLOC_POPULATE));
    REQUIRE(pop, "populate alloc: %s", dnuma_last_error());
    REQUIRE(dnuma_query_page(&pop[100], &pi) == 0, "query");
    CHECK(pi.state == DNUMA_PAGE_EXCLUSIVE && strcmp(pi.owner_host, ni->host) == 0,
          "populated page: %s owner %s", state_name(pi.state), pi.owner_host);
    uint64_t nonzero = 0;
    for (uint64_t i = 0; i < 64 * 4096 / 8; ++i) nonzero += pop[i] != 0;
    CHECK(nonzero == 0, "populated memory is not zero");

    // --- placement requests are never silently changed ---
    CHECK(dnuma_alloc(4096, 9999, 0) == nullptr, "allocation on a nonexistent node succeeded");
    CHECK(dnuma_thread_create(&th, 9999, remote_verify, c) != 0, "thread on a nonexistent node was created");
    printf("placement on nonexistent node refused: %s\n", dnuma_last_error());

    CHECK(dnuma_free(pop) == 0, "free: %s", dnuma_last_error());
    CHECK(dnuma_free(data) == 0, "free: %s", dnuma_last_error());
    CHECK(dnuma_free(c) == 0, "free: %s", dnuma_last_error());
    return finish("memory");
}
