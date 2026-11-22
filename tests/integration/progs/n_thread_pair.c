// SPDX-License-Identifier: GPL-2.0-only
// Native thread-placement acceptance test. Supply one master CPU and one
// slave CPU for the cluster test, or no arguments for a two-local-CPU baseline.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct shared {
    _Atomic int turn;
    int value; // protected by the release/acquire token
    pid_t pid;
};
struct worker {
    struct shared* shared;
    int side, cpu;
    pid_t tid;
};

static void die(const char* what) { perror(what); _exit(1); }

static size_t affinity(cpu_set_t** out) {
    size_t size = sizeof(cpu_set_t);
    for (;;) {
        *out = calloc(1, size);
        if (!*out) die("calloc affinity");
        if (sched_getaffinity(0, size, *out) == 0) return size;
        if (errno != EINVAL) die("sched_getaffinity");
        free(*out);
        if (size > UINT_MAX / 2) die("affinity mask exceeds Linux syscall length");
        size *= 2;
    }
}

static int cpu_arg(const char* text) {
    char* end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < 0 || value > INT_MAX) {
        fputs("expected a nonnegative Linux CPU ID\n", stderr);
        exit(2);
    }
    return (int)value;
}

static void* run(void* arg) {
    struct worker* w = arg;
    struct shared* s = w->shared;
    size_t count = (size_t)w->cpu + 1;
    size_t size = CPU_ALLOC_SIZE(count);
    cpu_set_t* mask = CPU_ALLOC(count);
    if (!mask) die("CPU_ALLOC");
    CPU_ZERO_S(size, mask);
    CPU_SET_S(w->cpu, size, mask);
    if (sched_setaffinity(0, size, mask)) {
        fprintf(stderr, "sched_setaffinity CPU %d: %s\n", w->cpu, strerror(errno));
        _exit(1);
    }
    CPU_FREE(mask);
    size = affinity(&mask);
    if (CPU_COUNT_S(size, mask) != 1 || !CPU_ISSET_S(w->cpu, size, mask)) {
        fprintf(stderr, "effective affinity differs from requested CPU %d\n", w->cpu);
        _exit(1);
    }
    free(mask);
    w->tid = (pid_t)syscall(SYS_gettid);
    for (int i = 0; i < 1024; ++i) {
        int turn;
        while ((turn = atomic_load_explicit(&s->turn, memory_order_acquire)) != w->side) {
            if (syscall(SYS_futex, &s->turn, FUTEX_WAIT_PRIVATE, turn, NULL, NULL, 0) < 0 &&
                errno != EAGAIN && errno != EINTR) die("FUTEX_WAIT_PRIVATE");
        }
        unsigned int cpu;
        if (syscall(SYS_getcpu, &cpu, NULL, NULL)) die("getcpu");
        if (getpid() != s->pid || cpu != (unsigned)w->cpu || s->value != 2*i + w->side) {
            fputs("PID, CPU, or shared heap sequence changed\n", stderr);
            _exit(1);
        }
        ++s->value;
        atomic_store_explicit(&s->turn, 1 - w->side, memory_order_release);
        if (syscall(SYS_futex, &s->turn, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) < 0) die("FUTEX_WAKE_PRIVATE");
    }
    return NULL;
}

int main(int argc, char** argv) {
    alarm(60);
    int cpu[2] = {-1, -1};
    if (argc == 3) {
        cpu[0] = cpu_arg(argv[1]); cpu[1] = cpu_arg(argv[2]);
        if (cpu[0] == cpu[1]) return 2;
    } else if (argc == 1) {
        cpu_set_t* mask;
        size_t size = affinity(&mask);
        int found = 0;
        for (size_t c = 0; c < size * CHAR_BIT && found < 2; ++c)
            if (CPU_ISSET_S(c, size, mask)) cpu[found++] = (int)c;
        free(mask);
        if (found < 2) { puts("requires two allowed CPUs"); return 77; }
    } else {
        fputs("usage: n_thread_pair [local-cpu remote-cpu]\n", stderr);
        return 2;
    }
    struct shared* s = calloc(1, sizeof(*s));
    if (!s) die("calloc shared heap");
    s->pid = getpid();
    atomic_init(&s->turn, 0);
    struct worker w[2] = {{s, 0, cpu[0], 0}, {s, 1, cpu[1], 0}};
    pthread_t other;
    int err = pthread_create(&other, NULL, run, &w[1]);
    if (err) { errno = err; die("pthread_create"); }
    run(&w[0]);
    err = pthread_join(other, NULL);
    if (err) { errno = err; die("pthread_join"); }
    if (w[0].tid == w[1].tid || s->value != 2048) return 1;
    printf("PASS pid %d, tid %d on CPU %d, tid %d on CPU %d, %d shared heap updates\n",
           (int)s->pid, (int)w[0].tid, cpu[0], (int)w[1].tid, cpu[1], s->value);
    free(s);
    return 0;
}
