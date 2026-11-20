// SPDX-License-Identifier: GPL-2.0-only
// Remote execution: run real code on every attached NUMA domain, verify the
// executing host and CPU from both the function's and the kernel's point of
// view, exchange data through distributed memory, and forward system calls
// to the home image.
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "check.h"

struct Task {
    dnuma_location where;
    const uint64_t* input;
    uint64_t begin, end;
    uint64_t sum;
    long native_pid;    // getpid() executed by the local kernel
    long forwarded_pid; // getpid() executed under the master's process context
    int out_fd;         // a descriptor that exists only in the home image
    long written;
    char line[128];
};

static uint64_t work(void* p) {
    Task* t = static_cast<Task*>(p);
    dnuma_where(&t->where);
    uint64_t s = 0;
    for (uint64_t i = t->begin; i < t->end; ++i) s += t->input[i] * t->input[i] + i;
    t->sum = s;
    t->native_pid = syscall(SYS_getpid);
    t->forwarded_pid = dnuma_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
    int n = snprintf(t->line, sizeof t->line, "node-task %s cpu %d sum %" PRIu64 "\n", t->where.host, t->where.cpu, s);
    // The buffer is distributed memory, so the master's kernel can read it;
    // the descriptor is the home image's.
    t->written = dnuma_syscall(SYS_write, t->out_fd, long(t->line), n, 0, 0, 0);
    return s;
}

int main() {
    REQUIRE(dnuma_active(), "must be started with dnuma-run");
    Topo t = load_topology();
    REQUIRE(t.first_slave >= 0, "no slave attached");
    const uint64_t kN = 1 << 20;
    uint64_t* input = static_cast<uint64_t*>(dnuma_alloc(kN * 8, DNUMA_NODE_ANY, 0));
    REQUIRE(input, "alloc: %s", dnuma_last_error());
    for (uint64_t i = 0; i < kN; ++i) input[i] = i ^ (i >> 3);

    char path[] = "/tmp/dnuma-t-exec-XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0, "mkstemp");

    int active = 0;
    for (int i = 0; i < t.count; ++i) active += t.nodes[i].present;
    Task* tasks = static_cast<Task*>(dnuma_alloc(sizeof(Task) * size_t(t.count), DNUMA_NODE_ANY, 0));
    dnuma_thread_t* th = new dnuma_thread_t[t.count];
    REQUIRE(tasks, "alloc");
    uint64_t slice = kN / uint64_t(active), next = 0;
    for (int i = 0; i < t.count; ++i) {
        if (!t.nodes[i].present) continue;
        tasks[i].input = input;
        tasks[i].begin = next;
        tasks[i].end = next + slice;
        tasks[i].out_fd = fd;
        next += slice;
        REQUIRE(dnuma_thread_create(&th[i], t.nodes[i].logical, work, &tasks[i]) == 0, "create on node %d: %s",
                t.nodes[i].logical, dnuma_last_error());
    }
    uint64_t total = 0, expect = 0;
    int remote = 0;
    for (int i = 0; i < t.count; ++i) {
        if (!t.nodes[i].present) continue;
        const dnuma_node_info& n = t.nodes[i];
        uint64_t r = 0;
        dnuma_thread_info info;
        REQUIRE(dnuma_thread_join(th[i], &r, &info) == 0, "join: %s", dnuma_last_error());
        Task& k = tasks[i];
        total += r;
        // Location as observed by the function itself ...
        CHECK(strcmp(k.where.host, n.host) == 0, "node %d: function ran on %s, expected %s", n.logical, k.where.host, n.host);
        CHECK(k.where.phys_node == n.phys_index, "node %d: ran on physical node %d, expected %d", n.logical, k.where.phys_node, n.phys_index);
        // ... and as reported by the hosting machine's daemon.
        CHECK(strcmp(info.host, n.host) == 0, "node %d: daemon reports host %s", n.logical, info.host);
        CHECK(info.phys_node == n.phys_index && info.logical_node == n.logical, "node %d: daemon reports node %d/%d", n.logical, info.logical_node, info.phys_node);
        CHECK(r == k.sum, "node %d: result mismatch", n.logical);
        // System calls: local ones see the local native process, forwarded
        // ones see the logical process on the master.
        CHECK(k.forwarded_pid == getpid(), "node %d: forwarded getpid %ld != home pid %d", n.logical, k.forwarded_pid, getpid());
        if (!n.is_master) {
            ++remote;
            CHECK(k.native_pid != getpid(), "node %d: native pid equals home pid on a slave?", n.logical);
        }
        CHECK(k.written == long(strlen(k.line)), "node %d: forwarded write returned %ld", n.logical, k.written);
        printf("node %d: %-10s cpu %-3d physical node %d tid %-8d native pid %-8ld forwarded pid %ld\n", n.logical, k.where.host,
               k.where.cpu, k.where.phys_node, info.tid, k.native_pid, k.forwarded_pid);
    }
    for (uint64_t i = 0; i < next; ++i) expect += input[i] * input[i] + i;
    CHECK(total == expect, "combined result %" PRIu64 " != %" PRIu64, total, expect);
    CHECK(remote > 0, "nothing ran remotely");

    // Every context's line must be in the file, written through the home fd.
    off_t size = lseek(fd, 0, SEEK_END);
    char* buf = new char[size_t(size) + 1];
    REQUIRE(pread(fd, buf, size_t(size), 0) == size, "pread");
    buf[size] = 0;
    int lines = 0;
    for (char* q = buf; *q; ++q) lines += *q == '\n';
    CHECK(lines == active, "%d lines in the home file, expected %d", lines, active);
    unlink(path);
    printf("ran on %d NUMA domains (%d remote); combined result verified\n", active, remote);
    return finish("exec");
}
