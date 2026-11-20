// SPDX-License-Identifier: GPL-2.0-only
// Coherence under concurrency, with threads running on both machines:
//   A. stale-read test   - a reader must never see an old value of a page
//                          after it has seen evidence of a newer one;
//   B. atomic counter    - lock-prefixed increments from both machines;
//   C. false sharing     - both machines write different words of one page;
//   D. distributed mutex - a plain counter protected by dnuma_mutex.
// Any divergence between copies, lost invalidation or double write
// ownership shows up as a wrong count.
#include <unistd.h>

#include "check.h"

// The fields are volatile so that every access in the loops below is a real
// load or store: the test is about what memory holds, and the compiler must
// not keep values in registers across iterations.
struct PageA {
    volatile uint64_t counter;
    char pad[4096 - 8];
};
struct PageB {
    volatile uint64_t ack;
    char pad[4096 - 8];
};

struct Shared {
    PageA* p; // written first
    PageB* q; // written second: "p.counter has reached at least this"
    uint64_t rounds;
    uint64_t violations;
    uint64_t reads;

    uint64_t* atomic_counter; // B
    uint64_t incs;

    volatile uint64_t* slots; // C: one word per thread, all in one page
    dnuma_mutex* mu; // D
    volatile uint64_t* plain;
};

struct Arg {
    Shared* s;
    int index;
};

static uint64_t writer(void* p) {
    Shared* s = static_cast<Arg*>(p)->s;
    for (uint64_t i = 1; i <= s->rounds; ++i) {
        s->p->counter = i;
        s->q->ack = i;
    }
    return 0;
}

static uint64_t reader(void* p) {
    Shared* s = static_cast<Arg*>(p)->s;
    uint64_t bad = 0, n = 0;
    for (;;) {
        uint64_t a = s->q->ack;     // evidence that counter >= a
        uint64_t c = s->p->counter; // must not be older than that
        if (c < a) ++bad;
        ++n;
        if (a >= s->rounds) break;
    }
    __atomic_fetch_add(&s->violations, bad, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&s->reads, n, __ATOMIC_SEQ_CST);
    return bad;
}

static uint64_t atomic_worker(void* p) {
    Shared* s = static_cast<Arg*>(p)->s;
    for (uint64_t i = 0; i < s->incs; ++i) __atomic_fetch_add(s->atomic_counter, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static uint64_t slot_worker(void* p) {
    Arg* a = static_cast<Arg*>(p);
    Shared* s = a->s;
    for (uint64_t i = 0; i < s->incs; ++i) s->slots[a->index] = s->slots[a->index] + 1;
    return 0;
}

static uint64_t mutex_worker(void* p) {
    Shared* s = static_cast<Arg*>(p)->s;
    for (uint64_t i = 0; i < s->incs; ++i) {
        dnuma_mutex_lock(s->mu);
        uint64_t v = *s->plain; // deliberately non-atomic read-modify-write
        *s->plain = v + 1;
        dnuma_mutex_unlock(s->mu);
    }
    return 0;
}

static void run_all(Shared* s, Arg* args, const int* nodes, int n, dnuma_fn fn, const char* what) {
    dnuma_thread_t th[16];
    for (int i = 0; i < n; ++i) {
        args[i].s = s;
        args[i].index = i;
        REQUIRE(dnuma_thread_create(&th[i], nodes[i], fn, &args[i]) == 0, "%s: create on node %d: %s", what, nodes[i], dnuma_last_error());
    }
    for (int i = 0; i < n; ++i) REQUIRE(dnuma_thread_join(th[i], nullptr, nullptr) == 0, "%s: join: %s", what, dnuma_last_error());
}

int main(int argc, char** argv) {
    REQUIRE(dnuma_active(), "must be started with dnuma-run");
    Topo t = load_topology();
    REQUIRE(t.first_slave >= 0, "no slave attached");
    int m = t.first_master, r = t.first_slave;
    uint64_t scale = argc > 1 ? strtoull(argv[1], nullptr, 10) : 1;

    Shared* s = static_cast<Shared*>(dnuma_alloc(sizeof(Shared), DNUMA_NODE_ANY, 0));
    Arg* args = static_cast<Arg*>(dnuma_alloc(sizeof(Arg) * 16, DNUMA_NODE_ANY, 0));
    REQUIRE(s && args, "alloc");
    s->p = static_cast<PageA*>(dnuma_alloc(4096, r, 0));
    s->q = static_cast<PageB*>(dnuma_alloc(4096, r, 0));
    s->atomic_counter = static_cast<uint64_t*>(dnuma_alloc(4096, DNUMA_NODE_ANY, 0));
    s->slots = static_cast<volatile uint64_t*>(dnuma_alloc(4096, DNUMA_NODE_ANY, 0));
    s->mu = static_cast<dnuma_mutex*>(dnuma_alloc(4096, DNUMA_NODE_ANY, 0));
    s->plain = static_cast<volatile uint64_t*>(dnuma_alloc(4096, DNUMA_NODE_ANY, 0));
    REQUIRE(s->p && s->q && s->atomic_counter && s->slots && s->mu && s->plain, "alloc: %s", dnuma_last_error());

    // --- A: one writer on the slave, readers on the master and the slave ---
    s->rounds = 3000 * scale;
    {
        dnuma_thread_t w, rd[3];
        int rnodes[3] = {m, m, r};
        for (int i = 0; i < 3; ++i) {
            args[i].s = s;
            REQUIRE(dnuma_thread_create(&rd[i], rnodes[i], reader, &args[i]) == 0, "reader: %s", dnuma_last_error());
        }
        args[3].s = s;
        REQUIRE(dnuma_thread_create(&w, r, writer, &args[3]) == 0, "writer: %s", dnuma_last_error());
        REQUIRE(dnuma_thread_join(w, nullptr, nullptr) == 0, "join");
        for (int i = 0; i < 3; ++i) REQUIRE(dnuma_thread_join(rd[i], nullptr, nullptr) == 0, "join");
    }
    CHECK(s->violations == 0, "%" PRIu64 " stale reads out of %" PRIu64, s->violations, s->reads);
    CHECK(s->p->counter == s->rounds && s->q->ack == s->rounds, "final values wrong");
    printf("A stale-read: %" PRIu64 " writes on the slave, %" PRIu64 " paired reads on both machines, %" PRIu64 " violations\n",
           s->rounds, s->reads, s->violations);

    // --- B: atomic increments from both machines ---
    s->incs = 1500 * scale;
    int four[4] = {m, r, m, r};
    run_all(s, args, four, 4, atomic_worker, "atomic");
    CHECK(*s->atomic_counter == 4 * s->incs, "atomic counter %" PRIu64 ", expected %" PRIu64, *s->atomic_counter, 4 * s->incs);
    printf("B atomic counter: %" PRIu64 " (expected %" PRIu64 ")\n", *s->atomic_counter, 4 * s->incs);

    // --- C: different words of the same page written on both machines ---
    run_all(s, args, four, 4, slot_worker, "slots");
    for (int i = 0; i < 4; ++i) CHECK(s->slots[i] == s->incs, "slot %d = %" PRIu64 ", expected %" PRIu64, i, s->slots[i], s->incs);
    printf("C same-page writers: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " (expected %" PRIu64 " each)\n", s->slots[0],
           s->slots[1], s->slots[2], s->slots[3], s->incs);

    // --- D: mutex shared by threads on both machines ---
    s->incs = 400 * scale;
    run_all(s, args, four, 4, mutex_worker, "mutex");
    CHECK(*s->plain == 4 * s->incs, "mutex-protected counter %" PRIu64 ", expected %" PRIu64, *s->plain, 4 * s->incs);
    printf("D distributed mutex: %" PRIu64 " (expected %" PRIu64 ")\n", *s->plain, 4 * s->incs);

    dnuma_page_info pi;
    if (dnuma_query_page(s->atomic_counter, &pi) == 0)
        printf("the counter page changed hands %" PRIu64 " times\n", pi.generation);
    return finish("coherence");
}
