// SPDX-License-Identifier: GPL-2.0-only
// Run on the master and ask dnumactl to export this PID to the CPU in argv[1].
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static unsigned cpu(void) {
    unsigned value;
    if (syscall(SYS_getcpu, &value, NULL, NULL) != 0) abort();
    return value;
}

static long private_dirty_kb(uintptr_t addr) {
    FILE* smaps = fopen("/proc/self/smaps", "r");
    if (!smaps) return -1;
    char* line = NULL;
    size_t size = 0;
    int inside = 0;
    long dirty = -1;
    while (getline(&line, &size, smaps) >= 0) {
        unsigned long start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) inside = start <= addr && addr < end;
        else if (inside && sscanf(line, "Private_Dirty: %ld", &dirty) == 1) break;
    }
    free(line);
    fclose(smaps);
    return dirty;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    unsigned target = strtoul(argv[1], NULL, 10);
    alarm(30);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    // Guard mappings keep these pages in their own smaps entry.
    char* area = mmap(NULL, 34 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED || mprotect(area + page, 32 * page, PROT_READ | PROT_WRITE) ||
        madvise(area + page, 32 * page, MADV_NOHUGEPAGE)) return 3;
    volatile char* table = area + page;
    int fd = memfd_create("constant", MFD_ALLOW_SEALING);
    if (fd < 0 || write(fd, "K", 1) != 1 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) return 4;
    volatile char* immutable = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
    if (immutable == MAP_FAILED) return 5;
    puts("READY");
    fflush(stdout);
    while (cpu() != target) {}
    if (*immutable != 'K') return 6;
    table[0] = 'A';
    for (size_t i = 1; i < 32; ++i) if (table[i * page] != 0) return 7;
    table[page] = 'B'; // a later write to a prefetched read-only copy
    // A wake uses the original address as its home-mm key. Linux also
    // validates word alignment and the bitset, even with no waiters.
    if (syscall(SYS_futex, table, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0) != 0 ||
        syscall(SYS_futex, table, FUTEX_WAKE_BITSET_PRIVATE, 1, 0, 0, FUTEX_BITSET_MATCH_ANY) != 0 ||
        syscall(SYS_futex, table + 1, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0) != -1 || errno != EINVAL ||
        syscall(SYS_futex, table, FUTEX_WAKE_BITSET_PRIVATE, 1, 0, 0, 0) != -1 || errno != EINVAL ||
        cpu() != target) return 13;
    if (mprotect(area + page, 32 * page, PROT_READ)) return 8;
    errno = 0;
    if (syscall(SYS_getcpu, table, NULL, NULL) != -1 || errno != EFAULT) return 9;
    if (mprotect(area + page, 32 * page, PROT_READ | PROT_WRITE)) return 10;
    if (cpu() != target) return 11;
    // A mapping with external writers must run at home before any user access.
    void* shared = mmap(NULL, page, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED || cpu() == target) return 12;
    long dirty = private_dirty_kb((uintptr_t)table);
    printf("cpu %u, private dirty %ld KiB\n", target, dirty);
    return table[0] != 'A' || table[page] != 'B' || dirty != (long)(2 * page / 1024);
}
