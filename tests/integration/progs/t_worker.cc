// SPDX-License-Identifier: GPL-2.0-only
// A long-running distributed application used by the drain, hard-disconnect
// and failure tests.
//
//   t_worker <logical-node> <megabytes> <seconds> [workers] [nomigrate]
//
// Worker contexts are started on <logical-node>. Each owns a slice of a
// distributed buffer (also placed there) and repeatedly adds 1 to every word
// of its slice, calling dnuma_migration_point() between passes. At the end
// every word of a slice must equal that worker's pass count: a page that was
// lost, duplicated or rolled back during a drain would break that.
// With "nomigrate" the workers never offer a migration point.
#include <time.h>
#include <unistd.h>

#include "check.h"

static const int kMaxWorkers = 16;

struct Ctl {
    volatile uint64_t stop;
    uint64_t no_migrate;
    uint64_t words_per_worker;
    uint64_t* data;
    struct Slot {
        volatile uint64_t passes;
        char host[DNUMA_HOST_LEN]; // where the worker last ran
        char pad[4096 - 8 - DNUMA_HOST_LEN];
    } w[kMaxWorkers];
};

struct Arg {
    Ctl* c;
    int index;
};

static uint64_t worker(void* p) {
    Arg* a = static_cast<Arg*>(p);
    Ctl* c = a->c;
    uint64_t* slice = c->data + uint64_t(a->index) * c->words_per_worker;
    uint64_t passes = 0;
    while (!c->stop) {
        for (uint64_t i = 0; i < c->words_per_worker; ++i) slice[i] += 1;
        ++passes;
        dnuma_location here;
        dnuma_where(&here);
        memcpy(c->w[a->index].host, here.host, sizeof here.host);
        c->w[a->index].passes = passes;
        if (!c->no_migrate) dnuma_migration_point(); // may resume on another machine
    }
    return passes;
}

static double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

int main(int argc, char** argv) {
    REQUIRE(dnuma_active(), "must be started with dnuma-run");
    REQUIRE(argc >= 4, "usage: t_worker <logical-node> <megabytes> <seconds> [workers]");
    int node = atoi(argv[1]);
    uint64_t mb = strtoull(argv[2], nullptr, 10);
    double seconds = atof(argv[3]);
    int workers = argc > 4 ? atoi(argv[4]) : 2;
    REQUIRE(workers >= 1 && workers <= kMaxWorkers, "1..%d workers", kMaxWorkers);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Ctl* c = static_cast<Ctl*>(dnuma_alloc(sizeof(Ctl), node, 0));
    Arg* args = static_cast<Arg*>(dnuma_alloc(sizeof(Arg) * kMaxWorkers, node, 0));
    uint64_t words = mb * 1024 * 1024 / 8;
    uint64_t* data = static_cast<uint64_t*>(dnuma_alloc(words * 8, node, 0));
    REQUIRE(c && args && data, "alloc: %s", dnuma_last_error());
    c->data = data;
    c->words_per_worker = words / uint64_t(workers);
    c->no_migrate = argc > 5 && strcmp(argv[5], "nomigrate") == 0;

    dnuma_thread_t th[kMaxWorkers];
    for (int i = 0; i < workers; ++i) {
        args[i].c = c;
        args[i].index = i;
        REQUIRE(dnuma_thread_create(&th[i], node, worker, &args[i]) == 0, "create on node %d: %s", node, dnuma_last_error());
    }
    printf("STARTED pid %d node %d %" PRIu64 " MiB %d workers\n", getpid(), node, mb, workers);

    double t0 = now_s();
    uint64_t last = 0;
    while (now_s() - t0 < seconds) {
        usleep(250000);
        uint64_t passes = 0;
        for (int i = 0; i < workers; ++i) passes += c->w[i].passes;
        char host[DNUMA_HOST_LEN];
        memcpy(host, c->w[0].host, sizeof host);
        host[sizeof host - 1] = 0;
        printf("TICK %.2f passes %" PRIu64 " (+%" PRIu64 ") worker0-on %s\n", now_s() - t0, passes, passes - last, host);
        last = passes;
    }
    c->stop = 1;

    uint64_t total = 0;
    unsigned migrations = 0;
    char final_host[DNUMA_HOST_LEN] = "";
    for (int i = 0; i < workers; ++i) {
        uint64_t passes = 0;
        dnuma_thread_info info;
        REQUIRE(dnuma_thread_join(th[i], &passes, &info) == 0, "join: %s", dnuma_last_error());
        total += passes;
        migrations += info.migrations;
        memcpy(final_host, info.host, sizeof final_host);
        // Every word of the slice was incremented exactly once per pass.
        uint64_t* slice = data + uint64_t(i) * c->words_per_worker;
        uint64_t bad = 0;
        for (uint64_t k = 0; k < c->words_per_worker; ++k) bad += slice[k] != passes;
        CHECK(bad == 0, "worker %d: %" PRIu64 " of %" PRIu64 " words do not equal its %" PRIu64 " passes", i, bad, c->words_per_worker, passes);
        CHECK(passes > 0, "worker %d made no progress", i);
    }
    dnuma_page_info pi;
    dnuma_query_page(&data[words / 2], &pi);
    printf("FINAL passes %" PRIu64 " migrations %u last-host %s data-owner %s\n", total, migrations, final_host, pi.owner_host);
    return finish("worker");
}
