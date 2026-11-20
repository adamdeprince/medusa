/* SPDX-License-Identifier: GPL-2.0-only */
// Minimal assertion helpers for the integration programs.
#pragma once

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dnuma/dnuma.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                            \
        }                                                            \
    } while (0)

#define REQUIRE(cond, ...)                                           \
    do {                                                             \
        if (!(cond)) {                                               \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                     \
            printf("\nRESULT FAIL\n");                               \
            exit(1);                                                 \
        }                                                            \
    } while (0)

static inline int finish(const char* name) {
    printf("RESULT %s %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", name, g_failures);
    fflush(stdout);
    return g_failures ? 1 : 0;
}

struct Topo {
    dnuma_node_info nodes[256];
    int count = 0;
    int first_slave = -1; // logical number of the first attached slave node
    int first_master = -1;
    char master_host[DNUMA_HOST_LEN] = "";
};

static inline Topo load_topology() {
    Topo t;
    // Ask for the count first; the table is sized generously and re-checked.
    t.count = dnuma_topology(t.nodes, 256);
    REQUIRE(t.count > 0 && t.count <= 256, "topology: %s", dnuma_last_error());
    for (int i = 0; i < t.count; ++i) {
        if (t.nodes[i].is_master && t.first_master < 0) {
            t.first_master = t.nodes[i].logical;
            memcpy(t.master_host, t.nodes[i].host, sizeof t.master_host);
        }
        if (!t.nodes[i].is_master && t.nodes[i].present && t.first_slave < 0) t.first_slave = t.nodes[i].logical;
    }
    return t;
}

static inline const dnuma_node_info* node_by_logical(const Topo& t, int logical) {
    for (int i = 0; i < t.count; ++i)
        if (t.nodes[i].logical == logical) return &t.nodes[i];
    return nullptr;
}

static inline const char* state_name(int s) {
    static const char* n[] = {"UNBACKED", "EXCLUSIVE", "SHARED", "LOST"};
    return s >= 0 && s < 4 ? n[s] : "?";
}
