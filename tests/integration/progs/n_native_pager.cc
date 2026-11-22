// SPDX-License-Identifier: GPL-2.0-only
// Live kernel test for an external pager and a native multithreaded target.
// The target reports one ordinary heap page, then uses ordinary loads,
// stores and read(2). Only the parent knows about userfaultfd.
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "node/native_pager.h"
#include "master/coherence.h"

using namespace dnuma;

static void fail(const char* what) {
    perror(what);
    _exit(1);
}
static void require(bool condition, const char* what) {
    if (!condition) { fprintf(stderr, "failed: %s\n", what); _exit(1); }
}
static void transfer(int fd, void* data, size_t size, bool writing) {
    auto* p = static_cast<char*>(data);
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail(writing ? "write pipe" : "read pipe");
        p += n;
        size -= size_t(n);
    }
}
static void send_word(int fd, uint64_t value) { transfer(fd, &value, sizeof(value), true); }
static uint64_t recv_word(int fd) {
    uint64_t value;
    transfer(fd, &value, sizeof(value), false);
    return value;
}
static void target(int commands, int replies) {
    size_t size = size_t(sysconf(_SC_PAGESIZE));
    void* storage = nullptr;
    require(posix_memalign(&storage, size, size * 3) == 0, "allocate aligned heap pages");
    auto* page = static_cast<unsigned char*>(storage) + size;
    memset(page, 0x2a, size);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> progress{0};
    std::thread sibling([&] {
        while (!stop.load(std::memory_order_relaxed))
            progress.fetch_add(1, std::memory_order_relaxed);
    });
    send_word(replies, uintptr_t(page));
    send_word(replies, syscall(SYS_gettid));
    while (true) {
        switch (recv_word(commands)) {
        case 1: send_word(replies, *page); break;
        case 2: *page += 1; send_word(replies, *page); break;
        case 3:
            transfer(commands, page, 1, false); // copy_to_user faults in kernel mode
            send_word(replies, *page);
            break;
        case 4: send_word(replies, progress.load()); break;
        case 0:
            stop.store(true);
            sibling.join();
            free(storage);
            _exit(0);
        default: _exit(2);
        }
    }
}

static void ctl(int fd, unsigned long nr, void* arg, const char* what) {
    int ret;
    do { ret = ioctl(fd, nr, arg); } while (ret < 0 && errno == EINTR);
    if (ret < 0) fail(what);
}
static void fault(int fd, uint64_t addr, uint64_t tid, uint64_t flags) {
    pollfd p{fd, POLLIN, 0};
    uffd_msg event{};
    for (;;) {
        int ret = poll(&p, 1, 10000);
        if (ret < 0 && errno == EINTR) continue;
        require(ret == 1 && (p.revents & POLLIN), "expected userfaultfd fault");
        ssize_t n = read(fd, &event, sizeof(event));
        // A signal can interrupt the faulting thread between poll and read.
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n != sizeof(event)) fail("read userfaultfd");
        break;
    }
    require(event.event == UFFD_EVENT_PAGEFAULT && event.arg.pagefault.address == addr &&
            event.arg.pagefault.feat.ptid == tid && event.arg.pagefault.flags == flags, "fault address, TID and flags");
}

struct Client {
    pid_t pid;
    int tx, rx, uffd;
    uint64_t address, tid;
};
static Client launch(const char* exe) {
    int commands[2], replies[2];
    if (pipe2(commands, O_CLOEXEC) || pipe2(replies, O_CLOEXEC)) fail("pipe");
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) fail("fork");
    if (!pid) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) fail("prctl PR_SET_PDEATHSIG");
        if (getppid() != parent) _exit(1);
        close(commands[1]); close(replies[0]);
        if (exe) {
            if (dup2(commands[0], STDIN_FILENO) < 0 || dup2(replies[1], STDOUT_FILENO) < 0) fail("dup2");
            close(commands[0]); close(replies[1]);
            execl(exe, exe, (char*)nullptr);
            fail("exec native target");
        }
        target(commands[0], replies[1]);
    }
    close(commands[0]); close(replies[1]);
    int tx = commands[1], rx = replies[0];
    uint64_t address = recv_word(rx), tid = recv_word(rx);
    size_t page_size = size_t(sysconf(_SC_PAGESIZE));
    int dev = open("/dev/userfaultfd", O_RDWR | O_CLOEXEC);
    if (dev < 0) fail("/dev/userfaultfd");
    dnuma_uffd_target request{int(syscall(SYS_pidfd_open, pid, 0)), O_CLOEXEC | O_NONBLOCK};
    if (request.pidfd < 0) fail("pidfd_open");
    pid_t unprivileged = fork();
    if (unprivileged < 0) fail("fork credential check");
    if (unprivileged == 0) {
        if (setuid(65534) != 0) _exit(2);
        int fd = ioctl(dev, DNUMA_UFFD_ATTACH, &request);
        _exit(fd < 0 && errno == EPERM ? 0 : 1);
    }
    int denied;
    require(waitpid(unprivileged, &denied, 0) == unprivileged &&
            WIFEXITED(denied) && WEXITSTATUS(denied) == 0, "unprivileged attach denied");
    int uffd = ioctl(dev, DNUMA_UFFD_ATTACH, &request);
    if (uffd < 0) fail("attach native pager");
    close(dev); close(request.pidfd);
    uffdio_api api{UFFD_API, UFFD_FEATURE_PAGEFAULT_FLAG_WP | UFFD_FEATURE_THREAD_ID, 0};
    ctl(uffd, UFFDIO_API, &api, "UFFDIO_API");
    uffdio_range range{address, page_size};
    uffdio_register reg{range, UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP, 0};
    ctl(uffd, UFFDIO_REGISTER, &reg, "register heap page");
    uffdio_range outside{address, page_size * 2};
    require(ioctl(uffd, DNUMA_UFFD_EVICT, &outside) < 0 && errno == EINVAL, "reject partially registered range");
    return {pid, tx, rx, uffd, address, tid};
}

static void finish(Client c) {
    send_word(c.tx, 0);
    int status;
    if (waitpid(c.pid, &status, 0) < 0) fail("waitpid");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "target exited normally");
    uffdio_range range{c.address, kPageSize};
    require(ioctl(c.uffd, DNUMA_UFFD_EVICT, &range) < 0 && errno == ESRCH, "old mm no longer live");
    close(c.uffd); close(c.rx); close(c.tx);
}

static void paging(Client c) {
    auto [pid, tx, rx, uffd, address, tid] = c;
    size_t page_size = kPageSize;
    uffdio_range range{address, page_size};
    send_word(tx, 4);
    uint64_t initial_progress = recv_word(rx);
    std::vector<unsigned char> saved(page_size);
    iovec remote{reinterpret_cast<void*>(address), page_size}, local{saved.data(), page_size};
    // Repeated ownership changes on an already populated application page.
    // Every cycle advances its contents as if a remote writer had held it.
    unsigned char expected = 0x2a;
    for (int i = 0; i < 128; ++i) {
        uffdio_writeprotect wp{range, UFFDIO_WRITEPROTECT_MODE_WP};
        ctl(uffd, UFFDIO_WRITEPROTECT, &wp, "freeze stores");
        if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != ssize_t(page_size)) fail("snapshot page");
        require(saved.front() == expected && saved.back() == 0x2a, "latest consistent page bytes");
        ctl(uffd, DNUMA_UFFD_EVICT, &range, "revoke page");
        saved.front() = ++expected;
        send_word(tx, 1);
        fault(uffd, address, tid, 0);
        uffdio_copy copy{address, uintptr_t(saved.data()), page_size, UFFDIO_COPY_MODE_WP, 0};
        ctl(uffd, UFFDIO_COPY, &copy, "grant read access");
        require(recv_word(rx) == expected, "reader saw new contents");
        bool kernel_store = i % 2 == 1;
        send_word(tx, kernel_store ? 3 : 2);
        ++expected;
        if (kernel_store) transfer(tx, &expected, 1, true);
        fault(uffd, address, tid, UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP);
        if (i % 4 == 0) {
            // A writer is already asleep on WP when the owner is revoked.
            // Have it retry against the missing PTE before granting write.
            ctl(uffd, DNUMA_UFFD_EVICT, &range, "revoke with a waiting writer");
            ctl(uffd, UFFDIO_WAKE, &range, "retry blocked writer");
            fault(uffd, address, tid, UFFD_PAGEFAULT_FLAG_WRITE);
            copy.mode = 0;
            ctl(uffd, UFFDIO_COPY, &copy, "return page to waiting writer");
        } else {
            wp.mode = 0;
            ctl(uffd, UFFDIO_WRITEPROTECT, &wp, "grant write access");
        }
        require(recv_word(rx) == expected, "writer resumed on correct page");
    }
    send_word(tx, 4);
    require(recv_word(rx) > initial_progress, "sibling thread made progress");
    ctl(uffd, UFFDIO_UNREGISTER, &range, "release registration");
    require(ioctl(uffd, DNUMA_UFFD_EVICT, &range) < 0 && errno == EINVAL, "reject unregistered page");
    finish(c);
    puts("PASS native heap paging (128 cycles, user stores and kernel writes, sibling running)");
}

// In the VM the two address spaces have real PTEs and execute real stores.
// Deliver the master's ordinary directory messages in-process in this test.
struct NativePair : CoherenceIo {
    struct Command { MachineId to; MsgType type; PageMsg msg; };
    Client image[2];
    Metrics metrics;
    Coherence directory{*this, metrics};
    std::deque<Command> commands;
    std::vector<unsigned char> staging[2];
    PageNo page;

    NativePair(Client home, Client peer) : image{home, peer}, page(page_of(home.address)) {
        directory.create_space(1, 0, page_of(UINT64_MAX) + 1);
        require(directory.register_native_region(1, page, 1, 0).is_ok(), "register native VMA");
        uffdio_range r{peer.address, kPageSize};
        ctl(peer.uffd, DNUMA_UFFD_EVICT, &r, "start replica absent");
    }
    Incarnation incarnation_of(MachineId) override { return 1; }
    bool machine_usable(MachineId m) override { return m < 2; }
    void coherence_failure(ProcId, const std::string& why) override { require(false, why.c_str()); }
    void send_page(MachineId to, MsgType type, const PageMsg& m) override { commands.push_back({to, type, m}); }

    void access(MachineId node, Access need, uint64_t flags) {
        auto& c = image[node];
        fault(c.uffd, c.address, c.tid, flags);
        PageMsg request{};
        request.proc = 1;
        request.page = page;
        request.access = need;
        directory.on_request(node, request, c.tid);
        while (!commands.empty()) {
            auto [to, type, m] = commands.front();
            commands.pop_front();
            auto& dst = image[to];
            uffdio_range range{dst.address, kPageSize};
            uffdio_writeprotect wp{range, UFFDIO_WRITEPROTECT_MODE_WP};
            if (type == MsgType::PageRevoke) {
                ctl(dst.uffd, UFFDIO_WRITEPROTECT, &wp, "freeze native replica");
                if (m.mode != RevokeMode::Invalidate) {
                    auto& bytes = staging[to];
                    bytes.resize(kPageSize);
                    iovec local{bytes.data(), bytes.size()}, remote{reinterpret_cast<void*>(dst.address), bytes.size()};
                    require(process_vm_readv(dst.pid, &local, 1, &remote, 1, 0) == ssize_t(bytes.size()), "stage native page");
                }
                if (m.mode == RevokeMode::Invalidate || m.mode == RevokeMode::InvalidateExport)
                    ctl(dst.uffd, DNUMA_UFFD_EVICT, &range, "invalidate native replica");
                directory.on_revoked(to, m);
            } else if (type == MsgType::PageGrant) {
                require(!m.status && m.source != GrantSource::Zero, "native grant preserves existing bytes");
                if (m.source == GrantSource::Local) {
                    wp.mode = m.access == Access::Write ? 0 : UFFDIO_WRITEPROTECT_MODE_WP;
                    ctl(dst.uffd, UFFDIO_WRITEPROTECT, &wp, "set granted access");
                } else {
                    auto& src = staging[m.src_machine];
                    require(src.size() == kPageSize, "source staged a full page");
                    uffdio_copy cp{dst.address, uintptr_t(src.data()), src.size(),
                                   m.access == Access::Write ? 0ULL : UFFDIO_COPY_MODE_WP, 0};
                    ctl(dst.uffd, UFFDIO_COPY, &cp, "install native grant");
                }
                directory.on_complete(to, m);
            } else if (type == MsgType::PageRelease) {
                staging[to].clear();
            } else {
                require(false, "unexpected directory operation");
            }
        }
    }
};

static void paired(const char* exe) {
    Client home = launch(nullptr), peer = launch(exe);
    NativePair pair(home, peer);
    unsigned char expected = 0x2a;
    for (unsigned i = 0; i < 256; ++i) {
        MachineId n = (i + 1) % 2;
        Client& c = pair.image[n];
        // A missing read makes two read-only replicas. The following store
        // must wait for invalidation of its peer before it can retire.
        send_word(c.tx, 1);
        pair.access(n, Access::Read, 0);
        require(recv_word(c.rx) == expected, "read peer's most recent store");
        send_word(c.tx, 2);
        pair.access(n, Access::Write, UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP);
        require(recv_word(c.rx) == ++expected, "native writer incremented shared byte");
    }
    finish(peer); finish(home);
    puts("PASS two live native address spaces, 256 directory-mediated writer handoffs");
}

int main(int argc, char** argv) {
    alarm(60);
    if (argc > 1 && strcmp(argv[1], "--pair") == 0 && argc <= 3)
        paired(argc == 3 ? argv[2] : nullptr);
    else if (argc <= 2)
        paging(launch(argc == 2 ? argv[1] : nullptr));
    else
        return 2;
    return 0;
}
