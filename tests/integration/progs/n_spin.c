// SPDX-License-Identifier: GPL-2.0-only
/*
 * An ordinary program: no dnuma library, dynamically linked, nothing special.
 * Used to test process export. It is CPU-bound, keeps a large buffer that it
 * rewrites and verifies, prints progress, and does file I/O in a directory
 * that exists only on the machine it was started on.
 *
 *   n_spin <seconds> <megabytes> [directory-for-files] [io-every-n-rounds]
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

int main(int argc, char** argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 5;
    size_t words = (argc > 2 ? strtoull(argv[2], NULL, 10) : 16) * 1024 * 1024 / 8;
    const char* dir = argc > 3 ? argv[3] : "/tmp";
    unsigned long io_every = argc > 4 ? strtoul(argv[4], NULL, 10) : 1;
    if (io_every == 0) io_every = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    pid_t pid = getpid();
    uint64_t* buf = malloc(words * 8);
    if (!buf) return 2;
    char path[512];
    snprintf(path, sizeof path, "%s/n_spin.%d", dir, (int)pid);
    printf("STARTED pid %d cpu %d\n", (int)pid, sched_getcpu());

    double t0 = now();
    int failures = 0, cpu_min = 1 << 30, cpu_max = -1;
    uint64_t round = 0;
    while (now() - t0 < seconds) {
        ++round;
        /* compute: rewrite the whole buffer, then verify it */
        for (size_t i = 0; i < words; ++i) buf[i] = mix(i ^ round);
        uint64_t sum = 0;
        for (size_t i = 0; i < words; ++i) {
            if (buf[i] != mix(i ^ round)) ++failures;
            sum += buf[i];
        }
        if (round % io_every != 0) continue;
        /* file I/O: write a slice, read it back through a second descriptor */
        size_t slice = words < 65536 ? words : 65536;
        int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd < 0 || write(fd, buf, slice * 8) != (ssize_t)(slice * 8)) ++failures;
        if (fd >= 0) close(fd);
        uint64_t* back = malloc(slice * 8);
        fd = open(path, O_RDONLY);
        if (fd < 0 || read(fd, back, slice * 8) != (ssize_t)(slice * 8) || memcmp(back, buf, slice * 8) != 0) ++failures;
        if (fd >= 0) close(fd);
        free(back);
        if (getpid() != pid) ++failures; /* identity must never change */
        int cpu = sched_getcpu();
        if (cpu < cpu_min) cpu_min = cpu;
        if (cpu > cpu_max) cpu_max = cpu;
        printf("TICK %.2f round %llu cpu %d sum %016llx\n", now() - t0, (unsigned long long)round, cpu, (unsigned long long)sum);
    }
    unlink(path);
    printf("FINAL rounds %llu cpus %d..%d failures %d\n", (unsigned long long)round, cpu_min, cpu_max, failures);
    printf("RESULT %s n_spin\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
