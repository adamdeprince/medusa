/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libdnuma - runtime interface for distributed processes.
 *
 * A program linked with libdnuma and started through dnuma-run becomes the
 * home image of a logical process whose threads and memory can be placed on
 * any NUMA domain of any attached machine.
 *
 * What this interface is, and is not:
 *   - Distributed allocations have one meaning everywhere: a pointer returned
 *     by dnuma_alloc() is valid, and refers to the same data, in every thread
 *     of the process on every machine. Access is by ordinary loads and
 *     stores; the pages follow the accesses.
 *   - Memory that is NOT distributed (static data, malloc heap, the main
 *     thread's stack, thread-local storage) is private to each machine's
 *     image of the process. Share data through dnuma_alloc() memory.
 *   - Threads are created with dnuma_thread_create(), not pthread_create().
 *     Transparent pthread support is not implemented.
 *
 * Unless noted otherwise, functions return 0 on success and a negative errno
 * value on failure; dnuma_last_error() then describes the failure.
 */
#ifndef DNUMA_H
#define DNUMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DNUMA_NODE_ANY (-1)

/* dnuma_alloc flags */
#define DNUMA_ALLOC_POPULATE 1u /* back the allocation physically on its node now */

/* Linux limits host names to 64 bytes (HOST_NAME_MAX); +1 for the terminator. */
#define DNUMA_HOST_LEN 65

enum dnuma_page_state { DNUMA_PAGE_UNBACKED = 0, DNUMA_PAGE_EXCLUSIVE = 1, DNUMA_PAGE_SHARED = 2, DNUMA_PAGE_LOST = 3 };

struct dnuma_node_info {
    int logical;      /* user-visible NUMA node number */
    int machine;      /* 0 = master */
    int phys_index;   /* kernel node number on that machine */
    int ncpus;
    int present;      /* machine attached and accepting work */
    int is_master;
    uint64_t mem_total, mem_free;
    char host[DNUMA_HOST_LEN];
};

struct dnuma_location {
    char host[DNUMA_HOST_LEN]; /* physical host executing the caller */
    int machine;
    int cpu;         /* CPU the caller is running on right now */
    int phys_node;   /* kernel NUMA node of that CPU */
    int is_home;     /* 1 in the home image on the master */
    uint64_t ctx;    /* execution context id, 0 for native threads */
};

typedef uint64_t dnuma_thread_t;
typedef uint64_t (*dnuma_fn)(void*);

#define DNUMA_THREAD_CANCELLED 1u

struct dnuma_thread_info {
    uint64_t result;
    char host[DNUMA_HOST_LEN]; /* host it last ran on */
    int cpu, tid;              /* as reported by that host's kernel at start */
    int logical_node, phys_node;
    unsigned migrations;       /* completed machine/node migrations */
    unsigned flags;
};

struct dnuma_page_info {
    int state;        /* enum dnuma_page_state */
    int owner_machine; /* -1 if none */
    char owner_host[DNUMA_HOST_LEN];
    uint64_t generation;
    int nreaders;
    int home_node;    /* logical node the allocation is placed on, -1 = any */
};

/* 1 if this process is running as a distributed process. */
int dnuma_active(void);
const char* dnuma_last_error(void);

/* ---- topology ---- */
/* Fills up to `max` entries; returns the total number of known nodes or -errno. */
int dnuma_topology(struct dnuma_node_info* out, int max);
int dnuma_where(struct dnuma_location* out);

/* ---- memory ---- */
/* Allocates page-granular distributed memory placed on logical NUMA node
 * `node` (DNUMA_NODE_ANY: wherever it is first touched). Returns NULL on
 * failure. The memory reads as zero until written. */
void* dnuma_alloc(size_t bytes, int node, unsigned flags);
int dnuma_free(void* addr);
/* Moves the authoritative copy of every backed page in the range to `node`'s
 * machine. *moved (if not NULL) receives the number of pages relocated. */
int dnuma_migrate_pages(void* addr, size_t len, int node, size_t* moved);
int dnuma_query_page(const void* addr, struct dnuma_page_info* out);

/* ---- execution ---- */
/* Starts fn(arg) as a new execution context on logical NUMA node `node`.
 * Placement is never silently changed: if the node cannot take the context,
 * the call fails. The context runs on its own stack in distributed memory. */
int dnuma_thread_create(dnuma_thread_t* thread, int node, dnuma_fn fn, void* arg);
int dnuma_thread_join(dnuma_thread_t thread, uint64_t* result, struct dnuma_thread_info* info);
/* Asks a context to move to `node`. It moves when it next reaches
 * dnuma_migration_point(). */
int dnuma_thread_migrate(dnuma_thread_t thread, int node);
/* A point at which the calling context may be suspended and resumed on
 * another machine. Thread-local storage does not move with it: do not keep
 * pointers to thread-local objects across this call. */
void dnuma_migration_point(void);

/* ---- operations under the master's process context ---- */
/* Executes a Linux system call in the home image on the master, i.e. with the
 * logical process's authoritative file descriptors, credentials and futex
 * queues. Pointer arguments must refer to distributed memory. In the home
 * image this is a direct system call. Returns the system call's result and
 * sets errno like syscall(2). */
long dnuma_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

/* Futex operations on a word in distributed memory, valid across machines. */
int dnuma_futex_wait(uint32_t* addr, uint32_t expected);
int dnuma_futex_wake(uint32_t* addr, int count);

/* A mutex that works between threads on different machines. It must live in
 * distributed memory and be zero-initialized. */
struct dnuma_mutex {
    uint32_t state;
};
void dnuma_mutex_lock(struct dnuma_mutex* m);
void dnuma_mutex_unlock(struct dnuma_mutex* m);

#ifdef __cplusplus
}
#endif

#endif /* DNUMA_H */
