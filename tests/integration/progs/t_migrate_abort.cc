// SPDX-License-Identifier: GPL-2.0-only
// Interrupted page transfer: starts moving its memory to a slave and expects
// the move to be cut short (the test harness delays the slave's page pulls
// and then disconnects it). Whatever happens to the transfer, every byte
// must still be readable and correct afterwards.
//
//   t_migrate_abort <logical-node> <megabytes>
#include <unistd.h>

#include "check.h"

int main(int argc, char** argv) {
    REQUIRE(dnuma_active(), "must be started with dnuma-run");
    REQUIRE(argc >= 3, "usage: t_migrate_abort <logical-node> <megabytes>");
    int node = atoi(argv[1]);
    uint64_t words = strtoull(argv[2], nullptr, 10) * 1024 * 1024 / 8;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Topo t = load_topology();
    uint64_t* data = static_cast<uint64_t*>(dnuma_alloc(words * 8, t.first_master, 0));
    REQUIRE(data, "alloc: %s", dnuma_last_error());
    for (uint64_t i = 0; i < words; ++i) data[i] = i * 0x100000001b3ull + 7;
    printf("STARTED pid %d; moving %" PRIu64 " pages to node %d\n", getpid(), words * 8 / 4096, node);

    size_t moved = 0;
    int rc = dnuma_migrate_pages(data, words * 8, node, &moved);
    printf("MIGRATE rc=%d moved=%zu (%s)\n", rc, moved, rc ? dnuma_last_error() : "ok");

    uint64_t bad = 0;
    for (uint64_t i = 0; i < words; ++i) bad += data[i] != i * 0x100000001b3ull + 7;
    CHECK(bad == 0, "%" PRIu64 " words lost or corrupted by the interrupted transfer", bad);
    dnuma_page_info pi;
    REQUIRE(dnuma_query_page(&data[words / 2], &pi) == 0, "query");
    printf("FINAL rc=%d moved=%zu owner=%s state=%s\n", rc, moved, pi.owner_host, state_name(pi.state));
    return finish("migrate_abort");
}
