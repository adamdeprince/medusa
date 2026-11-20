// SPDX-License-Identifier: GPL-2.0-only
// libdnuma: the runtime linked into every distributed program.
//
// One instance lives in each image of a logical process:
//   * home image   - the process the user launched on the master. Its main()
//                    runs normally; the runtime adds a service thread.
//   * shadow image - the same executable started by an agent on a slave. It
//                    never runs main(); its main thread is the service loop
//                    and it only hosts execution contexts placed there.
//
// Three rules keep the runtime deadlock-free under distributed paging:
//   R1. The service thread never touches distributed memory. It is the
//       thread that carries out unmap requests for the node's pager; if it
//       ever blocked on a page fault, the pager could wait on it forever.
//   R2. No runtime lock is ever held by a thread that is running on a
//       distributed stack. Execution contexts are fibers on distributed
//       stacks; whenever one calls into the runtime it first switches to its
//       carrier thread's private stack (see on_carrier()).
//   R3. Everything sent on the runtime socket is serialized into private
//       heap memory before the send lock is taken.
#include "dnuma/dnuma.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common/types.h"
#include "common/util.h"
#include "transport/messages.h"
#include "transport/stream.h"

extern "C" {
void dnuma_ctx_switch(void** save_sp, void* load_sp);
void dnuma_fiber_thunk(void);
void dnuma_fiber_main(uint64_t entry, uint64_t arg);
int __real_main(int, char**, char**);
int __wrap_main(int, char**, char**);
}

using namespace dnuma;

namespace {

enum : int { kFlagSuspend = 1, kFlagCancel = 2 };
enum class CtxState : int { Starting, Running, Calling, Suspended, Exited };

// One execution context hosted by this image. Lives in private heap memory.
struct Ctx {
    CtxId id = 0;
    uint64_t entry = 0, arg = 0, stack_base = 0, stack_size = 0;
    void* fiber_sp = nullptr;   // saved stack pointer of the fiber
    void* carrier_sp = nullptr; // saved stack pointer of the carrier thread
    std::atomic<int> flags{0};
    CtxState state = CtxState::Starting;
    uint64_t result = 0;
    bool cancelled = false;
    bool resume = false;        // true when adopting a migrated context
    sem_t start;
    std::vector<int32_t> cpus;
    void (*call_fn)(void*) = nullptr; // pending runtime call from the fiber
    void* call_arg = nullptr;
};

struct Waiter {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    Msg reply;
};

struct Proxy {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<SyscallMsg> queue;
};

struct Runtime {
    bool active = false;
    ImageRole role = ImageRole::Home;
    int sock = -1;
    ProcId proc = 0;
    MachineId machine = 0;
    char host[DNUMA_HOST_LEN] = "";
    uint64_t va_base = 0, va_size = 0;
    std::mutex send_mu;
    std::mutex mu; // guards the tables below
    std::unordered_map<uint64_t, Waiter*> waiters;
    uint64_t next_call = 1;
    std::unordered_map<CtxId, Ctx*> ctxs;
    std::unordered_map<CtxId, Proxy*> proxies;
};

Runtime* g = nullptr;
__thread Ctx* t_ctx = nullptr;
__thread char t_err[256];

void set_err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t_err, sizeof t_err, fmt, ap);
    va_end(ap);
}

[[noreturn]] void die(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dprintf(2, "dnuma runtime (pid %d): %s\n", getpid(), buf);
    // An image that cannot talk to its pager cannot be allowed to keep running.
    kill(getpid(), SIGKILL);
    _exit(113);
}

// R3: the message is fully built (private heap) before the lock is taken.
void rt_send(const Msg& m) {
    std::lock_guard<std::mutex> lk(g->send_mu);
    Status st = send_msg_blocking(g->sock, m);
    if (!st.is_ok()) die("lost connection to the node daemon: %s", st.str().c_str());
}

// ---------------------------------------------------------------------------
// Running runtime code on the carrier's private stack (rule R2).
// ---------------------------------------------------------------------------

template <class F> struct CallThunk {
    F* f;
    static void run(void* p) { (*static_cast<CallThunk*>(p)->f)(); }
};

// Executes f() on a private stack. From a native thread that is simply a
// call. From a fiber, control switches to the carrier thread, which runs f
// and switches back. f may read and write the fiber's memory freely - it
// holds no runtime lock while doing so.
template <class F> void on_carrier(F&& f) {
    Ctx* c = t_ctx;
    if (!c) {
        f();
        return;
    }
    CallThunk<F> thunk{&f};
    c->call_fn = &CallThunk<F>::run;
    c->call_arg = &thunk;
    c->state = CtxState::Calling;
    dnuma_ctx_switch(&c->fiber_sp, c->carrier_sp);
}

// ---------------------------------------------------------------------------
// Request/response with the master (via the node daemon).
// ---------------------------------------------------------------------------

// Must be called on a private stack. `body` is private heap memory.
Status rpc(AppOp op, std::vector<uint8_t> body, AppCallMsg* reply) {
    if (!g || !g->active) return Status::err(ENOTCONN, "not running as a distributed process (use dnuma-run)");
    Waiter* w = new Waiter;
    AppCallMsg call;
    call.proc = g->proc;
    call.op = op;
    call.body = std::move(body);
    {
        std::lock_guard<std::mutex> lk(g->mu);
        call.call = g->next_call++;
        g->waiters[call.call] = w;
    }
    Msg m = make_msg(MsgType::AppCall, call);
    rt_send(m);
    {
        std::unique_lock<std::mutex> lk(w->mu);
        w->cv.wait(lk, [&] { return w->done; });
    }
    bool ok = parse_msg(w->reply, reply);
    delete w;
    if (!ok) return Status::err(EPROTO, "malformed reply from master");
    if (reply->status != 0) return Status(reply->status, reply->error);
    return Status::ok();
}

int fail(const Status& st) {
    set_err("%s", st.message().empty() ? strerror(st.code()) : st.message().c_str());
    return -st.code();
}

void complete_waiter(uint64_t call, Msg&& m) {
    Waiter* w = nullptr;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        auto it = g->waiters.find(call);
        if (it == g->waiters.end()) return;
        w = it->second;
        g->waiters.erase(it);
    }
    std::lock_guard<std::mutex> lk(w->mu);
    w->reply = std::move(m);
    w->done = true;
    w->cv.notify_one();
}

// ---------------------------------------------------------------------------
// Execution contexts.
// ---------------------------------------------------------------------------

void send_exec(MsgType type, const Ctx* c, int status, uint64_t sp) {
    ExecMsg e;
    e.proc = g->proc;
    e.ctx = c->id;
    e.status = status;
    e.sp = sp;
    e.result = c->result;
    e.flags = c->cancelled ? kExecCancelled : 0;
    e.tid = int(syscall(SYS_gettid));
    e.cpu = sched_getcpu();
    rt_send(make_msg(type, e));
}

void* carrier_main(void* p) {
    Ctx* c = static_cast<Ctx*>(p);
    t_ctx = c;
    if (!c->cpus.empty()) {
        int maxcpu = 0;
        for (int cpu : c->cpus) maxcpu = std::max(maxcpu, cpu);
        cpu_set_t* set = CPU_ALLOC(size_t(maxcpu) + 1);
        size_t setsize = CPU_ALLOC_SIZE(size_t(maxcpu) + 1);
        if (set) {
            CPU_ZERO_S(setsize, set);
            for (int cpu : c->cpus) CPU_SET_S(size_t(cpu), setsize, set);
            sched_setaffinity(0, setsize, set);
            CPU_FREE(set);
        }
    }
    send_exec(MsgType::ExecStarted, c, 0, 0);

    if (!c->resume) {
        while (sem_wait(&c->start) != 0 && errno == EINTR) {}
        // Build the first frame on the distributed stack. These stores fault
        // the top stack page in on this machine.
        uint64_t top = (c->stack_base + c->stack_size) & ~15ull;
        uint64_t* sp = reinterpret_cast<uint64_t*>(top - 16);
        uint32_t csr[2] = {0, 0};
        __asm__ volatile("stmxcsr %0" : "=m"(csr[0]));
        uint16_t fcw = 0;
        __asm__ volatile("fnstcw %0" : "=m"(fcw));
        csr[1] = fcw;
        *--sp = reinterpret_cast<uint64_t>(&dnuma_fiber_thunk); // return address
        *--sp = 0;                                              // rbp
        *--sp = 0;                                              // rbx
        *--sp = c->entry;                                       // r12
        *--sp = c->arg;                                         // r13
        *--sp = 0;                                              // r14
        *--sp = 0;                                              // r15
        --sp;
        memcpy(sp, csr, sizeof csr);                            // mxcsr | x87 control word
        c->fiber_sp = sp;
    }

    for (;;) {
        c->state = CtxState::Running;
        dnuma_ctx_switch(&c->carrier_sp, c->fiber_sp);
        if (c->state != CtxState::Calling) break;
        c->call_fn(c->call_arg); // a runtime call made by the fiber, on our stack
    }

    {
        std::lock_guard<std::mutex> lk(g->mu);
        g->ctxs.erase(c->id);
    }
    if (c->state == CtxState::Exited) send_exec(MsgType::ExecExited, c, 0, 0);
    else send_exec(MsgType::ExecSuspended, c, 0, reinterpret_cast<uint64_t>(c->fiber_sp));
    sem_destroy(&c->start);
    delete c;
    return nullptr;
}

[[gnu::noinline]] void fiber_leave(CtxState state, uint64_t result, bool cancelled) {
    Ctx* c = t_ctx; // read on entry: the carrier may differ from call to call
    c->result = result;
    c->cancelled = cancelled;
    c->state = state;
    dnuma_ctx_switch(&c->fiber_sp, c->carrier_sp);
}

void start_carrier(Ctx* c) {
    {
        std::lock_guard<std::mutex> lk(g->mu);
        if (g->ctxs.count(c->id)) {
            delete c;
            return;
        }
        g->ctxs[c->id] = c;
    }
    sem_init(&c->start, 0, 0);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, carrier_main, c);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        {
            std::lock_guard<std::mutex> lk(g->mu);
            g->ctxs.erase(c->id);
        }
        ExecMsg e;
        e.proc = g->proc;
        e.ctx = c->id;
        e.status = rc;
        rt_send(make_msg(MsgType::ExecStarted, e));
        sem_destroy(&c->start);
        delete c;
    }
}

// ---------------------------------------------------------------------------
// System calls executed on behalf of remote contexts (home image only).
// Each remote context has one proxy thread here: that thread is the
// context's identity in the master's kernel.
// ---------------------------------------------------------------------------

void* proxy_main(void* p) {
    Proxy* px = static_cast<Proxy*>(p);
    for (;;) {
        SyscallMsg s;
        {
            std::unique_lock<std::mutex> lk(px->mu);
            px->cv.wait(lk, [&] { return !px->queue.empty(); });
            s = px->queue.front();
            px->queue.pop_front();
        }
        errno = 0;
        long ret = syscall(s.nr, s.args[0], s.args[1], s.args[2], s.args[3], s.args[4], s.args[5]);
        s.ret = ret;
        s.err = ret == -1 ? errno : 0;
        rt_send(make_msg(MsgType::SyscallComplete, s));
    }
    return nullptr;
}

void proxy_submit(const SyscallMsg& s) {
    Proxy* px = nullptr;
    {
        std::lock_guard<std::mutex> lk(g->mu);
        auto it = g->proxies.find(s.ctx);
        if (it != g->proxies.end()) px = it->second;
    }
    if (!px) {
        px = new Proxy;
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&th, &attr, proxy_main, px);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            delete px;
            SyscallMsg r = s;
            r.ret = -1;
            r.err = rc;
            rt_send(make_msg(MsgType::SyscallComplete, r));
            return;
        }
        std::lock_guard<std::mutex> lk(g->mu);
        g->proxies[s.ctx] = px;
    }
    std::lock_guard<std::mutex> lk(px->mu);
    px->queue.push_back(s);
    px->cv.notify_one();
}

// ---------------------------------------------------------------------------
// Service loop (rule R1: never touches distributed memory).
// ---------------------------------------------------------------------------

void service_loop() {
    for (;;) {
        Msg m;
        Status st = recv_msg_blocking(g->sock, &m);
        if (!st.is_ok()) die("node daemon connection closed: %s", st.str().c_str());
        close_fds(m.fds);
        switch (m.type) {
        case MsgType::RtZap: {
            RtZapMsg z;
            if (!parse_msg(m, &z)) break;
            // The kernel drops the PTEs and flushes the TLB of every CPU
            // running this address space before madvise returns.
            z.status = madvise(reinterpret_cast<void*>(z.addr), z.len, MADV_DONTNEED) == 0 ? 0 : errno;
            rt_send(make_msg(MsgType::RtZapDone, z));
            break;
        }
        case MsgType::ExecCreate:
        case MsgType::ExecMigrate: {
            ExecMsg e;
            if (!parse_msg(m, &e)) break;
            Ctx* c = new Ctx;
            c->id = e.ctx;
            c->entry = e.entry;
            c->arg = e.arg;
            c->stack_base = e.stack_base;
            c->stack_size = e.stack_size;
            c->cpus = e.cpus;
            if (m.type == MsgType::ExecMigrate) {
                c->resume = true;
                c->fiber_sp = reinterpret_cast<void*>(e.sp);
            }
            start_carrier(c);
            break;
        }
        case MsgType::ExecStart:
        case MsgType::ExecSuspend:
        case MsgType::ExecTerminate: {
            ExecMsg e;
            if (!parse_msg(m, &e)) break;
            std::lock_guard<std::mutex> lk(g->mu);
            auto it = g->ctxs.find(e.ctx);
            if (it == g->ctxs.end()) break;
            if (m.type == MsgType::ExecStart) sem_post(&it->second->start);
            else it->second->flags.fetch_or(m.type == MsgType::ExecSuspend ? kFlagSuspend : kFlagCancel);
            break;
        }
        case MsgType::AppReply: {
            AppCallMsg r;
            if (parse_msg(m, &r)) complete_waiter(r.call, std::move(m));
            break;
        }
        case MsgType::SyscallComplete: {
            SyscallMsg s;
            if (parse_msg(m, &s)) complete_waiter(s.call, std::move(m));
            break;
        }
        case MsgType::SyscallRequest: {
            SyscallMsg s;
            if (parse_msg(m, &s) && g->role == ImageRole::Home) proxy_submit(s);
            break;
        }
        default:
            break;
        }
    }
}

void* service_thread(void*) {
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, nullptr); // application signals go to application threads
    service_loop();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Image setup.
// ---------------------------------------------------------------------------

uint64_t read_canary() {
    uint64_t v;
    __asm__ volatile("movq %%fs:0x28, %0" : "=r"(v));
    return v;
}
void write_canary(uint64_t v) { __asm__ volatile("movq %0, %%fs:0x28" : : "r"(v)); }

// Creates a userfaultfd for this address space. Prefers a descriptor that
// can resolve kernel-mode faults (needed when a system call touches
// distributed memory); reports which kind it got.
int make_uffd(int dev_fd, bool* kernel_faults) {
    int flags = O_CLOEXEC | O_NONBLOCK;
    int fd = -1;
    *kernel_faults = true;
    if (dev_fd >= 0) fd = ioctl(dev_fd, USERFAULTFD_IOC_NEW, flags);
    if (fd < 0) fd = int(syscall(SYS_userfaultfd, flags));
    if (fd < 0) {
        *kernel_faults = false;
        fd = int(syscall(SYS_userfaultfd, flags | UFFD_USER_MODE_ONLY));
    }
    return fd;
}

Status setup_memory(int dev_fd, RtReadyMsg* ready, int* uffd_out) {
    void* want = reinterpret_cast<void*>(g->va_base);
    void* p = mmap(want, g->va_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return Status::sys("reserving distributed range at 0x%llx", (unsigned long long)g->va_base);
    if (p != want) return Status::err(EEXIST, "distributed range 0x%llx is already in use in this process", (unsigned long long)g->va_base);
    // 4 KiB is the coherence unit; huge pages would entangle neighbours.
    if (madvise(p, g->va_size, MADV_NOHUGEPAGE) != 0) return Status::sys("MADV_NOHUGEPAGE");
    // A forked child must not share (and silently diverge from) these pages.
    if (madvise(p, g->va_size, MADV_DONTFORK) != 0) return Status::sys("MADV_DONTFORK");

    bool kernel_faults = false;
    // Feature discovery needs its own descriptor: the API handshake is one-shot.
    int probe = make_uffd(dev_fd, &kernel_faults);
    if (probe < 0) return Status::sys("userfaultfd");
    uffdio_api api{};
    api.api = UFFD_API;
    if (ioctl(probe, UFFDIO_API, &api) != 0) {
        Status st = Status::sys("UFFDIO_API (probe)");
        close(probe);
        return st;
    }
    close(probe);
    uint64_t supported = api.features;
    if (!(supported & UFFD_FEATURE_PAGEFAULT_FLAG_WP))
        return Status::err(ENOSYS, "this kernel has no userfaultfd write-protect support");

    int fd = make_uffd(dev_fd, &kernel_faults);
    if (fd < 0) return Status::sys("userfaultfd");
    uffdio_api real{};
    real.api = UFFD_API;
    real.features = supported & (UFFD_FEATURE_PAGEFAULT_FLAG_WP | UFFD_FEATURE_THREAD_ID);
    if (ioctl(fd, UFFDIO_API, &real) != 0) {
        Status st = Status::sys("UFFDIO_API");
        close(fd);
        return st;
    }
    uffdio_register reg{};
    reg.range.start = g->va_base;
    reg.range.len = g->va_size;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    if (ioctl(fd, UFFDIO_REGISTER, &reg) != 0) {
        Status st = Status::sys("UFFDIO_REGISTER");
        close(fd);
        return st;
    }
    ready->uffd_features = real.features;
    ready->kernel_faults = kernel_faults;
    *uffd_out = fd;
    return Status::ok();
}

void handshake(ImageRole role, int argc, char** argv) {
    RtHelloMsg hello;
    hello.role = role;
    hello.pid = getpid();
    for (int i = 0; i < argc; ++i) hello.argv.emplace_back(argv[i]);
    hello.canary = read_canary();
    Status st = send_msg_blocking(g->sock, make_msg(MsgType::RtHello, hello));
    Msg m;
    if (st.is_ok()) st = recv_msg_blocking(g->sock, &m);
    RtWelcomeMsg w;
    if (!st.is_ok() || m.type != MsgType::RtWelcome || !parse_msg(m, &w)) {
        dprintf(2, "dnuma: cannot register with the node daemon: %s\n", st.is_ok() ? "bad reply" : st.str().c_str());
        _exit(125);
    }
    int dev_fd = m.fds.empty() ? -1 : m.fds[0];
    if (w.status != 0) {
        dprintf(2, "dnuma: registration refused: %s\n", w.text.c_str());
        _exit(125);
    }
    g->proc = w.proc;
    g->machine = w.machine;
    snprintf(g->host, sizeof g->host, "%s", w.hostname.c_str());
    g->va_base = w.va_base;
    g->va_size = w.va_size;

    RtReadyMsg ready;
    int uffd = -1;
    st = setup_memory(dev_fd, &ready, &uffd);
    if (dev_fd >= 0) close(dev_fd);

    if (role == ImageRole::Shadow) {
        // Same stack-protector canary as the home image, so that frames
        // created on one machine verify correctly after moving to another.
        write_canary(w.canary);
        // The agent started us with its privileges so that the userfaultfd
        // above can serve kernel-mode faults. Give them up now.
        if (geteuid() == 0 && w.uid != 0) {
            gid_t gid = w.gid;
            if (setgroups(1, &gid) != 0 || setgid(w.gid) != 0 || setuid(w.uid) != 0) {
                st = Status::sys("dropping privileges to uid %u", w.uid);
            }
        }
    }
    Msg out;
    if (!st.is_ok()) {
        ready.status = st.code();
        ready.text = st.str();
        out = make_msg(MsgType::RtReady, ready);
    } else {
        out = make_msg(MsgType::RtReady, ready);
        out.fds.push_back(uffd);
    }
    Status sst = send_msg_blocking(g->sock, out);
    if (uffd >= 0) close(uffd); // the node daemon holds it from here on
    if (!st.is_ok() || !sst.is_ok()) {
        dprintf(2, "dnuma: cannot set up distributed memory: %s\n", (!st.is_ok() ? st : sst).str().c_str());
        _exit(125);
    }
    if (!ready.kernel_faults)
        dprintf(2, "dnuma: warning: userfaultfd is user-mode only here; system calls that touch distributed memory "
                   "not currently present will fail with EFAULT\n");
    g->role = role;
    g->active = true;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry: every dnuma program is linked with -Wl,--wrap=main.
// ---------------------------------------------------------------------------

extern "C" int __wrap_main(int argc, char** argv, char** envp) {
    const char* shadow_fd = getenv("DNUMA_SHADOW_FD");
    const char* sock = getenv("DNUMA_SOCK");
    if (shadow_fd) {
        // If the agent dies, so do we: an image must never outlive the
        // daemon that pages for it and fences it.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        const char* ppid = getenv("DNUMA_PARENT_PID");
        if (ppid && getppid() != atoi(ppid)) _exit(125);
        g = new Runtime;
        g->sock = atoi(shadow_fd);
        handshake(ImageRole::Shadow, argc, argv);
        service_loop(); // never returns; main() is never run in a shadow image
    }
    if (sock && *sock) {
        g = new Runtime;
        Fd fd;
        Status st = unix_connect(sock, &fd);
        if (!st.is_ok()) {
            dprintf(2, "dnuma: cannot reach dnuma-master at %s: %s\n", sock, st.str().c_str());
            return 125;
        }
        g->sock = fd.release();
        // Children created by fork/exec must not inherit the registration.
        unsetenv("DNUMA_SOCK");
        handshake(ImageRole::Home, argc, argv);
        pthread_t th;
        if (pthread_create(&th, nullptr, service_thread, nullptr) != 0) {
            dprintf(2, "dnuma: cannot start the runtime service thread\n");
            return 125;
        }
        pthread_detach(th);
    }
    return __real_main(argc, argv, envp);
}

extern "C" void dnuma_fiber_main(uint64_t entry, uint64_t arg) {
    uint64_t r = reinterpret_cast<dnuma_fn>(entry)(reinterpret_cast<void*>(arg));
    fiber_leave(CtxState::Exited, r, false);
    __builtin_trap(); // an exited context is never resumed
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

extern "C" {

int dnuma_active(void) { return g && g->active; }
const char* dnuma_last_error(void) { return t_err; }

void dnuma_migration_point(void) {
    Ctx* c = t_ctx;
    if (!c) return;
    int f = c->flags.load(std::memory_order_relaxed);
    if (!f) return;
    if (f & kFlagCancel) {
        fiber_leave(CtxState::Exited, 0, true);
        __builtin_trap();
    }
    c->flags.fetch_and(~kFlagSuspend);
    // Everything this context needs is now on its (distributed) stack. It
    // continues from here on whichever machine resumes it; nothing after
    // this call may rely on thread-local state read before it.
    fiber_leave(CtxState::Suspended, 0, false);
}

int dnuma_where(struct dnuma_location* out) {
    if (!out) return -EINVAL;
    memset(out, 0, sizeof *out);
    unsigned cpu = 0, node = 0;
    syscall(SYS_getcpu, &cpu, &node, nullptr);
    out->cpu = int(cpu);
    out->phys_node = int(node);
    out->ctx = t_ctx ? t_ctx->id : 0;
    if (g && g->active) {
        memcpy(out->host, g->host, sizeof out->host);
        out->machine = int(g->machine);
        out->is_home = g->role == ImageRole::Home;
    } else {
        gethostname(out->host, sizeof out->host - 1);
        out->is_home = 1;
    }
    return 0;
}

int dnuma_topology(struct dnuma_node_info* out, int max) {
    int rc = 0;
    on_carrier([&] {
        AppCallMsg reply;
        TopologyReply topo;
        Status st = rpc(AppOp::Topology, {}, &reply);
        if (st.is_ok() && !decode_body(reply.body, &topo)) st = Status::err(EPROTO, "malformed topology");
        if (!st.is_ok()) {
            rc = fail(st);
            return;
        }
        rc = int(topo.nodes.size());
        for (int i = 0; i < rc && i < max && out; ++i) {
            const TopoNode& n = topo.nodes[size_t(i)];
            dnuma_node_info& o = out[i];
            memset(&o, 0, sizeof o);
            o.logical = n.logical;
            o.machine = int(n.machine);
            o.phys_index = n.phys_index;
            o.ncpus = int(n.ncpus);
            o.present = n.is_master || n.state == SlaveState::Active;
            o.is_master = n.is_master;
            o.mem_total = n.mem_total;
            o.mem_free = n.mem_free;
            snprintf(o.host, sizeof o.host, "%s", n.host.c_str());
        }
    });
    return rc;
}

void* dnuma_alloc(size_t bytes, int node, unsigned flags) {
    void* result = nullptr;
    on_carrier([&] {
        AllocReq rq;
        rq.bytes = bytes;
        rq.logical_node = node;
        rq.flags = flags;
        AppCallMsg reply;
        AddrMsg a;
        Status st = rpc(AppOp::Alloc, encode_body(rq), &reply);
        if (st.is_ok() && !decode_body(reply.body, &a)) st = Status::err(EPROTO, "malformed reply");
        if (!st.is_ok()) {
            errno = st.code();
            fail(st);
            return;
        }
        result = reinterpret_cast<void*>(a.addr);
    });
    return result;
}

int dnuma_free(void* addr) {
    int rc = 0;
    on_carrier([&] {
        AddrMsg a;
        a.addr = reinterpret_cast<uint64_t>(addr);
        AppCallMsg reply;
        Status st = rpc(AppOp::Free, encode_body(a), &reply);
        if (!st.is_ok()) rc = fail(st);
    });
    return rc;
}

int dnuma_migrate_pages(void* addr, size_t len, int node, size_t* moved) {
    int rc = 0;
    on_carrier([&] {
        AddrMsg a;
        a.addr = reinterpret_cast<uint64_t>(addr);
        a.len = len;
        a.logical_node = node;
        AppCallMsg reply;
        AddrMsg out;
        Status st = rpc(AppOp::MigratePages, encode_body(a), &reply);
        if (decode_body(reply.body, &out) && moved) *moved = size_t(out.a0);
        if (!st.is_ok()) rc = fail(st);
    });
    return rc;
}

int dnuma_query_page(const void* addr, struct dnuma_page_info* out) {
    int rc = 0;
    on_carrier([&] {
        AddrMsg a;
        a.addr = reinterpret_cast<uint64_t>(addr);
        AppCallMsg reply;
        PageInfoReply info;
        Status st = rpc(AppOp::PageInfo, encode_body(a), &reply);
        if (st.is_ok() && !decode_body(reply.body, &info)) st = Status::err(EPROTO, "malformed reply");
        if (!st.is_ok()) {
            rc = fail(st);
            return;
        }
        memset(out, 0, sizeof *out);
        out->state = int(info.state);
        out->owner_machine = info.owner == kNoMachine ? -1 : int(info.owner);
        snprintf(out->owner_host, sizeof out->owner_host, "%s", info.owner_host.c_str());
        out->generation = info.gen;
        out->nreaders = int(info.readers.size());
        out->home_node = info.home_logical;
    });
    return rc;
}

static void fill_info(const ThreadInfo& t, struct dnuma_thread_info* info) {
    if (!info) return;
    memset(info, 0, sizeof *info);
    info->result = t.result;
    snprintf(info->host, sizeof info->host, "%s", t.host.c_str());
    info->cpu = t.cpu;
    info->tid = t.tid;
    info->logical_node = t.logical_node;
    info->phys_node = t.phys_node;
    info->migrations = t.migrations;
    info->flags = t.flags;
}

int dnuma_thread_create(dnuma_thread_t* thread, int node, dnuma_fn fn, void* arg) {
    int rc = 0;
    on_carrier([&] {
        ThreadCreateReq rq;
        rq.logical_node = node;
        rq.entry = reinterpret_cast<uint64_t>(fn);
        rq.arg = reinterpret_cast<uint64_t>(arg);
        AppCallMsg reply;
        ThreadInfo info;
        Status st = rpc(AppOp::ThreadCreate, encode_body(rq), &reply);
        if (st.is_ok() && !decode_body(reply.body, &info)) st = Status::err(EPROTO, "malformed reply");
        if (!st.is_ok()) {
            rc = fail(st);
            return;
        }
        if (thread) *thread = info.ctx;
    });
    return rc;
}

int dnuma_thread_join(dnuma_thread_t thread, uint64_t* result, struct dnuma_thread_info* info) {
    int rc = 0;
    on_carrier([&] {
        AddrMsg a;
        a.a0 = thread;
        AppCallMsg reply;
        ThreadInfo t;
        Status st = rpc(AppOp::ThreadJoin, encode_body(a), &reply);
        if (st.is_ok() && !decode_body(reply.body, &t)) st = Status::err(EPROTO, "malformed reply");
        if (!st.is_ok()) {
            rc = fail(st);
            return;
        }
        if (result) *result = t.result;
        fill_info(t, info);
    });
    return rc;
}

int dnuma_thread_migrate(dnuma_thread_t thread, int node) {
    int rc = 0;
    on_carrier([&] {
        AddrMsg a;
        a.a0 = thread;
        a.logical_node = node;
        AppCallMsg reply;
        Status st = rpc(AppOp::ThreadMigrate, encode_body(a), &reply);
        if (!st.is_ok()) rc = fail(st);
    });
    return rc;
}

long dnuma_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    if (!g || !g->active || g->role == ImageRole::Home) return syscall(nr, a0, a1, a2, a3, a4, a5);
    long ret = -1;
    int err = ENOSYS;
    on_carrier([&] {
        Ctx* c = t_ctx;
        Waiter* w = new Waiter;
        SyscallMsg s;
        s.proc = g->proc;
        s.ctx = c ? c->id : 0;
        s.nr = nr;
        s.args = {uint64_t(a0), uint64_t(a1), uint64_t(a2), uint64_t(a3), uint64_t(a4), uint64_t(a5)};
        {
            std::lock_guard<std::mutex> lk(g->mu);
            s.call = g->next_call++;
            g->waiters[s.call] = w;
        }
        rt_send(make_msg(MsgType::SyscallRequest, s));
        {
            std::unique_lock<std::mutex> lk(w->mu);
            w->cv.wait(lk, [&] { return w->done; });
        }
        SyscallMsg r;
        if (parse_msg(w->reply, &r)) {
            ret = long(r.ret);
            err = r.err;
        }
        delete w;
    });
    if (ret == -1) errno = err;
    return ret;
}

int dnuma_futex_wait(uint32_t* addr, uint32_t expected) {
    long rc = dnuma_syscall(SYS_futex, long(addr), FUTEX_WAIT, long(expected), 0, 0, 0);
    return rc == 0 ? 0 : -errno;
}

int dnuma_futex_wake(uint32_t* addr, int count) {
    long rc = dnuma_syscall(SYS_futex, long(addr), FUTEX_WAKE, count, 0, 0, 0);
    return rc < 0 ? -errno : int(rc);
}

// Three-state futex mutex (0 free, 1 held, 2 held with waiters). The atomic
// operations act on a distributed page: the page is exclusively owned by one
// machine at a time, so they are atomic across machines as well. Waiting and
// waking go through the master's kernel, which sees every waiter.
void dnuma_mutex_lock(struct dnuma_mutex* m) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&m->state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        dnuma_futex_wait(&m->state, 2);
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    }
}

void dnuma_mutex_unlock(struct dnuma_mutex* m) {
    if (__atomic_exchange_n(&m->state, 0, __ATOMIC_RELEASE) == 2) dnuma_futex_wake(&m->state, 1);
}

} // extern "C"
