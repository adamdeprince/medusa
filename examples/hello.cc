// SPDX-License-Identifier: GPL-2.0-only
// Smallest end-to-end dnuma program: allocate distributed memory placed on a
// remote NUMA node, run a function there that fills it, and read the result
// back on the master through ordinary memory accesses.
//
//   dnuma-run ./dnuma-hello [logical-node]
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dnuma/dnuma.h"

struct Shared {
    dnuma_location where;  // filled in by the remote function
    uint64_t sum;
    uint64_t count;
    uint64_t data[];
};

static uint64_t fill(void* p) {
    Shared* s = static_cast<Shared*>(p);
    dnuma_where(&s->where); // which host and CPU is really executing this?
    uint64_t sum = 0;
    for (uint64_t i = 0; i < s->count; ++i) {
        s->data[i] = i * 2654435761u + 17;
        sum += s->data[i];
    }
    s->sum = sum;
    return sum;
}

static const char* state_name(int s) {
    static const char* n[] = {"UNBACKED", "EXCLUSIVE", "SHARED", "LOST"};
    return s >= 0 && s < 4 ? n[s] : "?";
}

int main(int argc, char** argv) {
    if (!dnuma_active()) {
        fprintf(stderr, "run me with: dnuma-run %s\n", argv[0]);
        return 2;
    }
    dnuma_node_info nodes[64];
    int n = dnuma_topology(nodes, 64);
    if (n < 0) {
        fprintf(stderr, "topology: %s\n", dnuma_last_error());
        return 1;
    }
    int target = -1;
    if (argc > 1) target = atoi(argv[1]);
    printf("logical NUMA topology (%d nodes):\n", n);
    for (int i = 0; i < n && i < 64; ++i) {
        printf("  node %-2d %-12s physical %d  %3d CPUs  %6.1f GiB  %s\n", nodes[i].logical, nodes[i].host,
               nodes[i].phys_index, nodes[i].ncpus, double(nodes[i].mem_total) / (1 << 30),
               nodes[i].is_master ? "master" : (nodes[i].present ? "slave" : "absent"));
        if (target < 0 && !nodes[i].is_master && nodes[i].present) target = nodes[i].logical;
    }
    if (target < 0) {
        fprintf(stderr, "no slave NUMA node is attached; attach one with dnumactl\n");
        return 1;
    }

    dnuma_location me;
    dnuma_where(&me);
    const uint64_t count = 100000;
    size_t bytes = sizeof(Shared) + count * sizeof(uint64_t);
    Shared* s = static_cast<Shared*>(dnuma_alloc(bytes, target, 0));
    if (!s) {
        fprintf(stderr, "dnuma_alloc: %s\n", dnuma_last_error());
        return 1;
    }
    printf("\nhome image on %s (cpu %d); allocated %zu bytes at %p placed on node %d\n", me.host, me.cpu, bytes,
           static_cast<void*>(s), target);
    s->count = count; // first touch of the header page happens here, on the master

    dnuma_thread_t t;
    if (dnuma_thread_create(&t, target, fill, s) != 0) {
        fprintf(stderr, "dnuma_thread_create: %s\n", dnuma_last_error());
        return 1;
    }
    uint64_t result = 0;
    dnuma_thread_info info;
    if (dnuma_thread_join(t, &result, &info) != 0) {
        fprintf(stderr, "dnuma_thread_join: %s\n", dnuma_last_error());
        return 1;
    }
    dnuma_page_info before;
    dnuma_query_page(&s->data[count / 2], &before);
    printf("remote function returned %" PRIu64 "; kernel on %s reports it ran as tid %d on cpu %d (physical node %d)\n",
           result, info.host, info.tid, info.cpu, info.phys_node);
    printf("a data page before the master reads it: %s, owner %s\n", state_name(before.state), before.owner_host);

    // Verify on the master by plain loads: each first access to a page
    // faults it in from the remote machine over RDMA.
    uint64_t sum = 0, bad = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (s->data[i] != i * 2654435761u + 17) ++bad;
        sum += s->data[i];
    }
    dnuma_page_info after;
    dnuma_query_page(&s->data[count / 2], &after);
    printf("the same page after: %s, owner %s, %d reader(s)\n", state_name(after.state), after.owner_host, after.nreaders);
    printf("function ran on host '%s' cpu %d (its own report)\n", s->where.host, s->where.cpu);
    bool ok = bad == 0 && sum == result && sum == s->sum && strcmp(s->where.host, me.host) != 0;
    printf("%s: %" PRIu64 " mismatching words, checksum %s\n", ok ? "OK" : "FAILED", bad, sum == result ? "matches" : "DIFFERS");
    dnuma_free(s);
    return ok ? 0 : 1;
}
