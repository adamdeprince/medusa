// SPDX-License-Identifier: GPL-2.0-only
#include "agent/body.h"

#include <algorithm>
#include <cinttypes>
#include <condition_variable>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"

namespace dnuma {

namespace {
constexpr uint64_t kPg = 4096;
constexpr uint32_t kReadahead = 16;      // pages requested per fault, within one mapping;
                                         // doubled for each fault that continues the previous
                                         // request, up to kXMaxPagesPerRequest
// How fast two machines' clocks may drift apart: each may be slewed by up to
// 500 ppm by the kernel's clock discipline (MAXFREQ in the NTP code).
constexpr uint64_t kClockDriftPpm = 1000;
constexpr uint64_t kMaxIo = 8ull << 20;  // one read/write call carries at most this much;
                                         // the caller sees an ordinary short transfer
constexpr uint64_t kPathMax = 4096;      // PATH_MAX
constexpr uint64_t kIovMax = 1024;       // IOV_MAX
inline uint64_t pg(uint64_t a) { return a & ~(kPg - 1); }

struct KIovec {
    uint64_t base, len;
};

uint64_t arg_of(const user_regs_struct& r, int i) {
    switch (i) {
    case 0: return r.rdi;
    case 1: return r.rsi;
    case 2: return r.rdx;
    case 3: return r.r10;
    case 4: return r.r8;
    default: return r.r9;
    }
}
bool transfers_bytes(int64_t nr) { // calls whose length may legally be shortened
    return nr == SYS_read || nr == SYS_write || nr == SYS_pread64 || nr == SYS_pwrite64;
}
} // namespace

// ============================ clocks ============================

uint64_t ClockSync::error_at(const Sample& s, uint64_t now) {
    return s.rtt / 2 + (now - s.at) / 1000000 * kClockDriftPpm;
}

void ClockSync::add(const XClocks& sent, const XClocks& master, const XClocks& received) {
    const int mono = xclock_index(CLOCK_MONOTONIC);
    if (received[size_t(mono)] < sent[size_t(mono)]) return;
    Sample n;
    n.at = received[size_t(mono)];
    n.rtt = received[size_t(mono)] - sent[size_t(mono)];
    // The master read its clocks somewhere between our two readings; taking
    // the middle is wrong by at most half the round trip.
    for (size_t i = 0; i < kXClocks; ++i) n.off[i] = int64_t(master[i]) - int64_t(sent[i] / 2 + received[i] / 2);
    std::lock_guard<std::mutex> lk(mu_);
    // Errors of all samples grow at the same rate, so one that is no better
    // than the new one now never will be.
    uint64_t err = error_at(n, n.at);
    std::erase_if(samples_, [&](const Sample& s) { return error_at(s, n.at) >= err; });
    samples_.push_back(n);
}

bool ClockSync::get(Offsets* off, uint64_t* err_ns) const {
    uint64_t now = now_ns();
    std::lock_guard<std::mutex> lk(mu_);
    const Sample* best = nullptr;
    for (const Sample& s : samples_)
        if (!best || error_at(s, now) < error_at(*best, now)) best = &s;
    if (!best) return false;
    *off = best->off;
    *err_ns = error_at(*best, now);
    return true;
}

bool ClockSync::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return samples_.empty();
}

BodyRunner::BodyRunner(Reactor& reactor, Metrics& metrics, std::string stub_path, std::function<void(Msg)> to_master,
                       std::shared_ptr<const ClockSync> clocks)
    : reactor_(reactor), metrics_(metrics), stub_path_(std::move(stub_path)), to_master_(std::move(to_master)),
      clocks_(std::move(clocks)) {}

BodyRunner::~BodyRunner() {
    kill_all();
    if (group_ > 0) {
        kill(group_, SIGKILL);
        int st;
        while (waitpid(group_, &st, 0) < 0 && errno == EINTR) {}
    }
}

void BodyRunner::ensure_group() {
    if (group_ > 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        prctl(PR_SET_NAME, "dnuma-bodies"); // not to be mistaken for the agent itself
        for (;;) pause();
    }
    if (pid > 0) {
        setpgid(pid, pid); // also from this side, so it is in place before any body joins
        group_ = pid;
    }
}

BodyRunner::Body* BodyRunner::find(uint64_t xid) {
    auto it = bodies_.find(xid);
    return it == bodies_.end() ? nullptr : it->second.get();
}

// Makes the stopped body execute one real system call of ours (always a
// memory-management call on itself) through the stub's "syscall; int3" gadget.
//
// It must work from any stop, including a system-call entry stop under
// PTRACE_SYSEMU. There, resuming with PTRACE_CONT lets the kernel finish
// skipping the body's own (emulated) call - orig_rax = -1 makes that a no-op
// that leaves rax alone - and return to user mode at the gadget, where our
// call executes natively and the breakpoint stops the body again. The saved
// registers are restored afterwards.
Status BodyRunner::inject(Body& b, long nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, long* ret) {
    user_regs_struct saved, r;
    DNUMA_TRY(pt_getregs(b.pid, &saved));
    r = saved;
    r.rip = b.gadget;
    r.rax = uint64_t(nr);
    r.orig_rax = uint64_t(-1);
    r.rdi = a0;
    r.rsi = a1;
    r.rdx = a2;
    r.r10 = a3;
    r.r8 = a4;
    r.r9 = a5;
    DNUMA_TRY(pt_setregs(b.pid, r));
    for (;;) {
        if (ptrace(PTRACE_CONT, b.pid, 0, 0) != 0) return Status::sys("PTRACE_CONT body %d", b.pid);
        int st = 0;
        DNUMA_TRY(pt_wait_stop(b.pid, &st));
        if (!is_event_stop(st) && stop_signal(st) == SIGTRAP) break; // the gadget's breakpoint
        // Anything else (a stray stop signal, say) is not for the body to act on.
    }
    DNUMA_TRY(pt_getregs(b.pid, &r));
    if (r.rip != b.gadget + 3) return Status::err(EIO, "body did not return through the gadget");
    *ret = long(r.rax);
    saved.orig_rax = uint64_t(-1); // whatever call the body was entering has been disposed of
    return pt_setregs(b.pid, saved);
}

// ============================ lifecycle ============================

Status BodyRunner::spawn(Body& b, const XBeginMsg& x) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return Status::sys("socketpair");
    Fd ours(sv[0]), theirs(sv[1]);
    std::string uid = std::to_string(x.uid), gid = std::to_string(x.gid);
    ensure_group();
    pid_t group = group_;
    const char* stub = stub_path_.c_str();
    const char* uid_arg = uid.c_str();
    const char* gid_arg = gid.c_str();
    int their_fd = theirs.get();
    // vfork, not fork: the agent's address space is large (registered memory,
    // page buffers) and duplicating its page tables for every body would stall
    // all of the agent's threads. The child borrows this thread's memory until
    // it executes the stub, and does nothing but make system calls.
    pid_t pid = vfork();
    if (pid < 0) return Status::sys("vfork");
    if (pid == 0) {
        if (group > 0) setpgid(0, group);
        if (dup2(their_fd, 3) < 0) _exit(126);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execl(stub, "dnuma-body", uid_arg, gid_arg, static_cast<char*>(nullptr));
        _exit(127);
    }
    b.pid = pid;
    if (group > 0) setpgid(pid, group);
    by_pid_[pid] = b.xid;
    theirs.reset();
    int st = 0;
    DNUMA_TRY(pt_wait_stop(pid, &st)); // stopped at exec
    if (ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) != 0)
        return Status::sys("PTRACE_SETOPTIONS");
    if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) return Status::sys("PTRACE_CONT");

    // The stub sends its userfaultfd, then traps.
    char byte = 0;
    iovec iov{&byte, 1};
    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int))];
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof cbuf;
    ssize_t n;
    do n = recvmsg(ours.get(), &mh, MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    cmsghdr* c = n == 1 ? CMSG_FIRSTHDR(&mh) : nullptr;
    if (!c || c->cmsg_type != SCM_RIGHTS) return Status::err(EIO, "body stub failed to start (%s)", stub_path_.c_str());
    int ufd;
    memcpy(&ufd, CMSG_DATA(c), sizeof ufd);
    b.uffd.reset(ufd);
    DNUMA_TRY(pt_wait_stop(pid, &st));
    user_regs_struct r;
    DNUMA_TRY(pt_getregs(pid, &r));
    b.gadget = r.rax;

    // Clear out everything the kernel and the stub put in the address space,
    // keeping only the page with the gadget.
    std::vector<ProcVma> maps;
    DNUMA_TRY(read_proc_maps(pid, &maps));
    uint64_t keep_start = 0, keep_end = 0;
    for (const ProcVma& v : maps) {
        if (b.gadget >= v.start && b.gadget < v.end) {
            keep_start = v.start;
            keep_end = v.end;
            continue;
        }
        if (v.name == "[vsyscall]") continue;
        long ret = 0;
        DNUMA_TRY(inject(b, SYS_munmap, v.start, v.end - v.start, 0, 0, 0, 0, &ret));
        if (ret != 0) return Status::err(int(-ret), "cannot clear body mapping %s", v.name.c_str());
    }
    b.mem.reset(open(("/proc/" + std::to_string(pid) + "/mem").c_str(), O_RDWR | O_CLOEXEC));
    if (!b.mem.valid()) return Status::sys("open body memory");

    for (const XVma& v : x.vmas) {
        if (v.start < keep_end && keep_start < v.end)
            return Status::err(EEXIST, "the process uses the address range reserved for the body stub (0x%" PRIx64 ")", keep_start);
        DNUMA_TRY(map_vma(b, v));
    }
    if (x.local_cpu >= 0) {
        // Run where the master says it runs, so its view of the CPU is true.
        cpu_set_t* set = CPU_ALLOC(size_t(x.local_cpu) + 1);
        size_t sz = CPU_ALLOC_SIZE(size_t(x.local_cpu) + 1);
        if (set) {
            CPU_ZERO_S(sz, set);
            CPU_SET_S(size_t(x.local_cpu), sz, set);
            sched_setaffinity(pid, sz, set);
            CPU_FREE(set);
        }
    }
    user_regs_struct regs;
    if (!regs_from_bytes(x.regs, &regs)) return Status::err(EPROTO, "malformed register set");
    regs.orig_rax = uint64_t(-1);
    DNUMA_TRY(pt_setregs(pid, regs));
    DNUMA_TRY(pt_set_xstate(pid, x.xstate));
    b.rseq = x.rseq;
    uint64_t xid = b.xid;
    return reactor_.add_fd(b.uffd.get(), EPOLLIN, [this, xid](uint32_t) { on_uffd(xid); });
}

void BodyRunner::begin(const XBeginMsg& x) {
    auto body = std::make_unique<Body>();
    Body& b = *body;
    b.xid = x.xid;
    b.cpu = x.cpu;
    b.node = x.node;
    b.clock_floor = x.clocks;
    b.t0 = now_ns();
    bodies_[x.xid] = std::move(body);
    count_.store(uint32_t(bodies_.size()));
    Status st = spawn(b, x);
    if (b.pid > 0) {
        std::lock_guard<std::mutex> lk(pids_mu_);
        pids_[x.xid] = b.pid;
    }
    XReadyMsg r;
    r.xid = x.xid;
    r.body_pid = b.pid;
    if (!st.is_ok()) {
        LOG_WARN("export %" PRIu64 ": cannot create body: %s", x.xid, st.str().c_str());
        r.status = st.code();
        r.text = st.str();
        destroy(x.xid);
        to_master_(make_msg(MsgType::XReady, r));
        return;
    }
    metrics_.add("bodies_started");
    LOG_INFO("export %" PRIu64 ": body pid %d runs home pid %d (%zu mappings)", x.xid, b.pid, x.home_pid, x.vmas.size());
    to_master_(make_msg(MsgType::XReady, r));
    resume(b);
}

void BodyRunner::destroy(uint64_t xid) {
    auto it = bodies_.find(xid);
    if (it == bodies_.end()) return;
    Body& b = *it->second;
    if (b.uffd.valid()) reactor_.del_fd(b.uffd.get());
    for (auto p = by_pid_.begin(); p != by_pid_.end();) p = p->second == xid ? by_pid_.erase(p) : std::next(p);
    if (b.pid > 0) {
        kill(b.pid, SIGKILL);
        int st;
        while (waitpid(b.pid, &st, __WALL) < 0 && errno == EINTR) {}
    }
    bodies_.erase(it);
    count_.store(uint32_t(bodies_.size()));
    std::lock_guard<std::mutex> lk(pids_mu_);
    pids_.erase(xid);
}

void BodyRunner::pids(std::vector<std::pair<uint64_t, pid_t>>* out) const {
    std::lock_guard<std::mutex> lk(pids_mu_);
    out->insert(out->end(), pids_.begin(), pids_.end());
}

void BodyRunner::kill_all() {
    std::vector<uint64_t> ids;
    for (auto& [id, b] : bodies_) ids.push_back(id);
    for (uint64_t id : ids) destroy(id);
}

uint64_t BodyRunner::cpu_time_ns(Body& b) {
    std::string s = read_attr("/proc/" + std::to_string(b.pid) + "/stat");
    size_t paren = s.rfind(')');
    if (paren == std::string::npos) return 0;
    std::vector<std::string> f = split(s.substr(paren + 2), ' ');
    if (f.size() < 13) return 0;
    uint64_t ticks = strtoull(f[11].c_str(), nullptr, 10) + strtoull(f[12].c_str(), nullptr, 10);
    return ticks * (1000000000ull / uint64_t(sysconf(_SC_CLK_TCK)));
}

void BodyRunner::resume(Body& b) {
    if (b.rseq && b.pages.count(pg(b.rseq)) && !b.rseq_done) {
        // The C library reads "which CPU am I on" from this kernel-maintained
        // area instead of asking. Keep it truthful for the body: it runs on
        // the CPU the master presents it as.
        uint32_t ids[2] = {uint32_t(b.cpu < 0 ? 0 : b.cpu), uint32_t(b.cpu < 0 ? 0 : b.cpu)}; // cpu_id_start, cpu_id
        if (write_out(b, b.rseq, reinterpret_cast<const uint8_t*>(ids), sizeof ids) == Step::Done) b.rseq_done = true;
    }
    b.st = St::Running;
    // SYSEMU: run until the next system call and stop *instead of* executing it.
    if (ptrace(PTRACE_SYSEMU, b.pid, 0, 0) != 0) LOG_ERROR("export %" PRIu64 ": PTRACE_SYSEMU: %s", b.xid, strerror(errno));
}

void BodyRunner::go_home(Body& b, XReason reason, int64_t nr, const std::string& text) {
    uint64_t xid = b.xid;
    XHomeMsg h;
    h.xid = xid;
    h.reason = reason;
    h.nr = nr;
    h.text = text;
    // A body waiting on its own system call is stopped at that call's entry.
    h.at_syscall = (b.st == St::AwaitPages || b.st == St::AwaitReply || b.st == St::Stopped) && reason != XReason::Fault;
    if (b.st == St::Running) {
        // Stop it wherever it is - computing, or blocked waiting for a page.
        // (A stop signal is reported to us, the tracer, before anything else
        // happens to it; the body is discarded afterwards anyway.)
        int st = 0;
        if (kill(b.pid, SIGSTOP) != 0 || !pt_wait_stop(b.pid, &st).is_ok()) {
            XSimpleMsg g;
            g.xid = xid;
            g.text = "body died while being stopped";
            to_master_(make_msg(MsgType::XGone, g));
            return destroy(xid);
        }
        h.at_syscall = is_syscall_stop(st);
    }
    user_regs_struct r{};
    (void)pt_getregs(b.pid, &r);
    h.regs = regs_to_bytes(r);
    (void)pt_get_xstate(b.pid, &h.xstate);
    h.utime_ns = cpu_time_ns(b);
    h.clock_last = b.clock_last;
    to_master_(make_msg(MsgType::XHome, h));

    uint64_t sent = 0;
    if (reason != XReason::Exit) {
        // Everything the body modified goes home; nothing else needs to.
        XPagePushMsg push;
        push.xid = xid;
        auto flush = [&] {
            if (push.addrs.empty()) return;
            sent += push.addrs.size();
            to_master_(make_msg(MsgType::XPagePush, push));
            push.addrs.clear();
            push.data.clear();
        };
        for (const auto& [page, state] : b.pages) {
            if (state != 2) continue;
            size_t off = push.data.size();
            push.data.resize(off + kPg);
            if (!mem_read(b.mem.get(), page, push.data.data() + off, kPg).is_ok()) {
                push.data.resize(off);
                LOG_ERROR("export %" PRIu64 ": cannot read modified page 0x%" PRIx64, xid, page);
                continue;
            }
            push.addrs.push_back(page);
            if (push.addrs.size() == 64) flush();
        }
        flush();
    }
    XSimpleMsg done;
    done.xid = xid;
    done.count = sent;
    to_master_(make_msg(MsgType::XHomeDone, done));
    metrics_.add("bodies_sent_home");
    metrics_.add("body_pages_sent_home", sent);
    LOG_INFO("export %" PRIu64 ": body pid %d sent home (%s%s%s): %" PRIu64 " syscalls forwarded, %" PRIu64 " pages in, %" PRIu64
             " modified pages out",
             xid, b.pid, reason == XReason::Requested ? "requested" : reason == XReason::Syscall ? "system call" : reason == XReason::Fault ? "fault" : reason == XReason::Exit ? "exit" : "error",
             text.empty() ? "" : ": ", text.c_str(), b.syscalls, b.pages_in, sent);
    destroy(xid);
}

// ============================ events ============================

void BodyRunner::on_sigchld() {
    if (group_ <= 0) return;
    for (int budget = 256; budget > 0; --budget) {
        int st = 0;
        pid_t pid = waitpid(-group_, &st, WNOHANG | __WALL | __WNOTHREAD);
        if (pid <= 0) return;
        auto it = by_pid_.find(pid);
        if (it == by_pid_.end()) continue;
        if (Body* b = find(it->second)) handle_stop(*b, st);
    }
    reactor_.post([this] { on_sigchld(); }); // more than a batch: let others run first
}

void BodyRunner::handle_stop(Body& b, int status) {
    if (!WIFSTOPPED(status)) {
        LOG_WARN("export %" PRIu64 ": body pid %d died unexpectedly (status 0x%x)", b.xid, b.pid, status);
        XSimpleMsg g;
        g.xid = b.xid;
        g.text = "body process died";
        to_master_(make_msg(MsgType::XGone, g));
        uint64_t xid = b.xid;
        b.pid = 0; // already reaped
        return destroy(xid);
    }
    if (is_syscall_stop(status)) {
        if (b.st == St::Running) on_syscall(b);
        return;
    }
    if (is_event_stop(status)) return resume(b);
    int sig = stop_signal(status);
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP) {
        // A synchronous fault. The instruction is re-executed at home, where
        // the kernel raises it with the right details for the real process.
        b.st = St::Stopped;
        go_home(b, XReason::Fault, -1, format("signal %d in user code", sig));
        return;
    }
    // Signals meant for the process arrive at home, not here.
    resume(b);
}

void BodyRunner::on_uffd(uint64_t xid) {
    for (;;) {
        Body* b = find(xid);
        if (!b || !b->uffd.valid()) return;
        uffd_msg msg;
        ssize_t n = ::read(b->uffd.get(), &msg, sizeof msg);
        if (n != ssize_t(sizeof msg)) return;
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;
        uint64_t page = pg(msg.arg.pagefault.address);
        ++b->faults;
        if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {
            mark_dirty(*b, page); // first write to a clean page
            continue;
        }
        if (b->pages.count(page)) {
            uffdio_range r{page, kPg};
            ioctl(b->uffd.get(), UFFDIO_WAKE, &r);
            continue;
        }
        request_page(*b, page, msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE);
    }
}

void BodyRunner::mark_dirty(Body& b, uint64_t page) {
    uffdio_writeprotect wp{};
    wp.range.start = page;
    wp.range.len = kPg;
    wp.mode = 0;
    ioctl(b.uffd.get(), UFFDIO_WRITEPROTECT, &wp);
    b.pages[page] = 2;
}

const XVma* BodyRunner::find_vma(Body& b, uint64_t addr) {
    auto it = b.vmas.upper_bound(addr);
    if (it == b.vmas.begin()) return nullptr;
    --it;
    return addr < it->second.end ? &it->second : nullptr;
}

bool BodyRunner::range_writable(Body& b, uint64_t addr, uint64_t len) {
    if (len == 0) return true;
    if (addr + len < addr) return false;
    for (uint64_t a = addr; a < addr + len;) {
        const XVma* v = find_vma(b, a);
        if (!v || !(v->prot & kVmWrite)) return false;
        a = v->end;
    }
    return true;
}

void BodyRunner::request_page(Body& b, uint64_t page, bool for_write) {
    for (const auto& [start, count] : b.requested)
        if (page >= start && page < start + uint64_t(count) * kPg) return;
    const XVma* v = find_vma(b, page);
    // A fault right after the previous request's last page is a sequential
    // walk: ask for twice as much as last time.
    uint32_t window = page == b.ra_next ? std::min(b.ra_window * 2, kXMaxPagesPerRequest) : kReadahead;
    uint32_t n = 1;
    while (v && n < window) {
        uint64_t next = page + uint64_t(n) * kPg;
        if (next >= v->end || b.pages.count(next) || b.requested.count(next)) break;
        ++n;
    }
    b.ra_next = page + uint64_t(n) * kPg;
    b.ra_window = std::max(n, kReadahead);
    b.requested[page] = n;
    b.req_t0[page] = now_ns();
    if (for_write) b.req_write[page] = true;
    XPageMsg rq;
    rq.xid = b.xid;
    rq.addr = page;
    rq.count = n;
    to_master_(make_msg(MsgType::XPageReq, rq));
}

void BodyRunner::on_page(Body& b, const XPageView& p) {
    b.requested.erase(p.addr);
    if (auto t = b.req_t0.find(p.addr); t != b.req_t0.end()) {
        metrics_.observe("body_page_fetch_ns", now_ns() - t->second);
        b.req_t0.erase(t);
    }
    if (p.status != 0 || p.count == 0 || p.len < uint64_t(p.count) * kPg) {
        // The address is not valid in the process at home.
        if (b.st == St::AwaitPages) {
            b.have_reply = false;
            return finish_syscall(b, -EFAULT);
        }
        return go_home(b, XReason::Fault, -1, format("invalid memory access at 0x%" PRIx64, p.addr));
    }
    // A fault caused by a write usually belongs to a run of writes (a buffer
    // being filled). Installing that batch writable, counted as modified,
    // saves one write-protect fault per page; the cost is that pages the
    // body turns out not to write are sent home again unnecessarily.
    bool written = b.req_write.erase(p.addr) != 0;
    for (uint32_t i = 0; i < p.count; ++i) {
        uint64_t page = p.addr + uint64_t(i) * kPg;
        if (b.pages.count(page)) continue;
        const XVma* v = find_vma(b, page);
        bool dirty = written && v && (v->prot & kVmWrite);
        uffdio_copy cp{};
        cp.dst = page;
        cp.src = reinterpret_cast<uint64_t>(p.data + size_t(i) * kPg);
        cp.len = kPg;
        cp.mode = dirty ? 0 : UFFDIO_COPY_MODE_WP; // otherwise clean until the body writes to it
        if (ioctl(b.uffd.get(), UFFDIO_COPY, &cp) != 0 && errno != EEXIST) {
            LOG_ERROR("export %" PRIu64 ": UFFDIO_COPY 0x%" PRIx64 ": %s", b.xid, page, strerror(errno));
            continue;
        }
        b.pages[page] = dirty ? 2 : 1;
        ++b.pages_in;
    }
    metrics_.add("body_pages_in", p.count);
    if (b.st == St::AwaitPages) {
        if (b.have_reply) apply_reply(b);
        else continue_syscall(b);
    }
}

BodyRunner::Step BodyRunner::need_range(Body& b, uint64_t addr, uint64_t len) {
    if (len == 0) return Step::Done;
    if (addr + len < addr) return Step::Fault;
    Step result = Step::Done;
    for (uint64_t page = pg(addr); page < addr + len; page += kPg) {
        if (b.pages.count(page)) continue;
        if (!find_vma(b, page)) return Step::Fault;
        request_page(b, page);
        result = Step::Wait;
    }
    return result;
}

// ============================ address space ============================

Status BodyRunner::map_vma(Body& b, const XVma& v) {
    if (v.end <= v.start || (v.start & (kPg - 1)) || (v.end & (kPg - 1))) return Status::err(EINVAL, "bad mapping");
    vma_remove(b, v.start, v.end);
    long ret = 0;
    DNUMA_TRY(inject(b, SYS_mmap, v.start, v.end - v.start, v.prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                     uint64_t(-1), 0, &ret));
    if (uint64_t(ret) != v.start) return Status::err(ret < 0 ? int(-ret) : EIO, "cannot mirror mapping at 0x%" PRIx64, v.start);
    uffdio_register reg{};
    reg.range.start = v.start;
    reg.range.len = v.end - v.start;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP;
    if (ioctl(b.uffd.get(), UFFDIO_REGISTER, &reg) != 0) return Status::sys("UFFDIO_REGISTER 0x%" PRIx64, v.start);
    b.vmas[v.start] = v;
    return Status::ok();
}

void BodyRunner::vma_remove(Body& b, uint64_t start, uint64_t end) {
    // Split mappings that straddle the boundaries, then drop what is inside.
    for (uint64_t edge : {start, end}) {
        auto it = b.vmas.upper_bound(edge);
        if (it == b.vmas.begin()) continue;
        --it;
        if (it->second.start < edge && edge < it->second.end) {
            XVma tail = it->second;
            tail.start = edge;
            it->second.end = edge;
            b.vmas[edge] = tail;
        }
    }
    for (auto it = b.vmas.lower_bound(start); it != b.vmas.end() && it->second.start < end;) it = b.vmas.erase(it);
    for (auto it = b.pages.begin(); it != b.pages.end();) it = (it->first >= start && it->first < end) ? b.pages.erase(it) : std::next(it);
    for (auto it = b.requested.begin(); it != b.requested.end();) it = (it->first >= start && it->first < end) ? b.requested.erase(it) : std::next(it);
    for (auto it = b.req_write.begin(); it != b.req_write.end();) it = (it->first >= start && it->first < end) ? b.req_write.erase(it) : std::next(it);
}

// ============================ system calls ============================

void BodyRunner::on_syscall(Body& b) {
    b.stop_t0 = now_ns();
    if (!pt_getregs(b.pid, &b.sregs).is_ok()) return;
    uint64_t args[6];
    for (int i = 0; i < 6; ++i) args[i] = arg_of(b.sregs, i);
    b.desc = &describe_syscall(int64_t(b.sregs.orig_rax), args);
    b.have_reply = false;
    b.st = St::Stopped;
    ++b.syscalls;
    switch (b.desc->cls) {
    case SysClass::Home:
        metrics_.add("body_syscalls_sent_home");
        return go_home(b, XReason::Syscall, int64_t(b.sregs.orig_rax), b.desc->name);
    case SysClass::Exit:
        return go_home(b, XReason::Exit, int64_t(b.sregs.orig_rax), "");
    default:
        return continue_syscall(b);
    }
}

BodyRunner::Step BodyRunner::local_syscall(Body& b, long* ret) {
    int64_t nr = int64_t(b.sregs.orig_rax);
    *ret = 0;
    if (nr == SYS_arch_prctl) {
        uint64_t code = b.sregs.rdi, addr = b.sregs.rsi;
        if (code == 0x1002) b.sregs.fs_base = addr;       // ARCH_SET_FS
        else if (code == 0x1001) b.sregs.gs_base = addr;  // ARCH_SET_GS
        else if (code == 0x1003 || code == 0x1004) {      // ARCH_GET_FS / ARCH_GET_GS
            uint64_t v = code == 0x1003 ? b.sregs.fs_base : b.sregs.gs_base;
            if (!range_writable(b, addr, 8)) return Step::Fault;
            return write_out(b, addr, reinterpret_cast<const uint8_t*>(&v), 8);
        } else {
            *ret = -EINVAL; // other sub-functions (CET, LAM, AMX permission) are not offered to bodies
        }
        return Step::Done;
    }
    if (nr == SYS_getcpu) {
        uint32_t cpu = b.cpu < 0 ? 0 : uint32_t(b.cpu), node = b.node < 0 ? 0 : uint32_t(b.node);
        for (auto [addr, val] : {std::pair<uint64_t, uint32_t>{b.sregs.rdi, cpu}, {b.sregs.rsi, node}}) {
            if (!addr) continue;
            if (!range_writable(b, addr, 4)) return Step::Fault;
            Step s = write_out(b, addr, reinterpret_cast<const uint8_t*>(&val), 4);
            if (s != Step::Done) return s;
        }
        return Step::Done;
    }
    return Step::Done; // sched_yield, futex wake with no possible waiter, advisory madvise: nothing to do
}

// Reading a system-wide clock: answered here, from this machine's clock and
// the measured difference to the master's (ClockSync). Returns false if the
// call has to go to the master after all.
//
// What the process may rely on is preserved:
//   * a clock never shows less than the process has already seen - neither
//     at home before it was exported (clock_floor) nor here (clock_last);
//     the master in turn does not let the process continue at home before
//     its own clocks have passed clock_last;
//   * the wall clocks follow the master's, including when it is set.
bool BodyRunner::local_time(Body& b, long* ret, Step* step) {
    int64_t nr = int64_t(b.sregs.orig_rax);
    int idx = -1;
    uint64_t ptr = 0;
    switch (nr) {
    case SYS_clock_gettime:
        idx = xclock_index(int64_t(int32_t(b.sregs.rdi))); // CPU-time and device clocks: not ours to answer
        ptr = b.sregs.rsi;
        break;
    case SYS_gettimeofday:
        if (b.sregs.rsi) return false; // the obsolete time-zone argument
        idx = xclock_index(CLOCK_REALTIME);
        ptr = b.sregs.rdi;
        break;
    case SYS_time:
        idx = xclock_index(CLOCK_REALTIME);
        ptr = b.sregs.rdi;
        break;
    default:
        return false;
    }
    ClockSync::Offsets off;
    uint64_t err = 0;
    if (idx < 0 || !clocks_ || !clocks_->get(&off, &err)) return false;
    size_t i = size_t(idx);
    timespec ts{};
    clock_gettime(kXClockIds[i], &ts);
    uint64_t v = uint64_t(int64_t(uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec)) + off[i]);
    uint64_t seen = std::max(b.clock_floor[i], b.clock_last[i]);
    if (v < seen) {
        // Within the measurement error: hold the clock still. A wall clock
        // further back than that has really been set back on the master.
        bool settable = kXClockIds[i] == CLOCK_REALTIME || kXClockIds[i] == CLOCK_TAI;
        if (!settable || seen - v <= 2 * err) v = seen;
    }
    b.clock_last[i] = v;
    metrics_.add("body_time_calls_local");

    int64_t out[2] = {int64_t(v / 1000000000ull), int64_t(v % 1000000000ull)};
    size_t len = sizeof out;
    *ret = 0;
    *step = Step::Done;
    if (nr == SYS_gettimeofday) out[1] /= 1000; // struct timeval
    if (nr == SYS_time) {
        *ret = long(out[0]);
        len = sizeof out[0];
    }
    if (!ptr) {
        if (nr == SYS_clock_gettime) *step = Step::Fault;
        return true;
    }
    if (!range_writable(b, ptr, len)) {
        *step = Step::Fault;
        return true;
    }
    *step = write_out(b, ptr, reinterpret_cast<const uint8_t*>(out), len);
    return true;
}

BodyRunner::Step BodyRunner::gather(Body& b, XSyscallMsg* out) {
    const SysDesc& d = *b.desc;
    int64_t nr = int64_t(b.sregs.orig_rax);
    out->xid = b.xid;
    out->nr = nr;
    for (int i = 0; i < 6; ++i) out->args[size_t(i)] = arg_of(b.sregs, i);
    Step result = Step::Done;
    auto merge = [&](Step s) {
        if (s == Step::Fault) result = Step::Fault;
        else if (s == Step::Wait && result == Step::Done) result = Step::Wait;
        return s == Step::Done;
    };
    for (int i = 0; i < 6 && result != Step::Fault; ++i) {
        const ArgDesc& a = d.a[i];
        uint64_t ptr = out->args[size_t(i)];
        if (!is_pointer(a.kind) || ptr == 0) continue;
        XBlob blob;
        blob.arg = uint8_t(i);
        switch (a.kind) {
        case ArgKind::Str: {
            bool terminated = false;
            for (uint64_t at = ptr; at < ptr + kPathMax && !terminated;) {
                if (!merge(need_range(b, at, 1))) break;
                uint64_t chunk = std::min(pg(at) + kPg, ptr + kPathMax) - at;
                size_t off = blob.data.size();
                blob.data.resize(off + chunk);
                if (!mem_read(b.mem.get(), at, blob.data.data() + off, chunk).is_ok()) return Step::Fault;
                const void* nul = memchr(blob.data.data() + off, 0, chunk);
                if (nul) {
                    blob.data.resize(size_t(static_cast<const uint8_t*>(nul) - blob.data.data()) + 1);
                    terminated = true;
                }
                at += chunk;
            }
            if (result != Step::Done) break;
            out->in.push_back(std::move(blob)); // unterminated => the master answers ENAMETOOLONG
            break;
        }
        case ArgKind::In:
        case ArgKind::SIn:
        case ArgKind::SInOut:
        case ArgKind::PollFds: {
            uint64_t len = a.kind == ArgKind::In ? out->args[a.len] : a.kind == ArgKind::PollFds ? out->args[a.len] * 8 : a.size;
            if (a.kind == ArgKind::In && transfers_bytes(nr) && len > kMaxIo) {
                len = kMaxIo;
                out->args[a.len] = len;
            }
            if (len > kMaxIo) return Step::Fault;
            if (carries_out(a.kind) && !range_writable(b, ptr, len)) return Step::Fault;
            if (!merge(need_range(b, ptr, len))) break;
            blob.data.resize(len);
            if (len && !mem_read(b.mem.get(), ptr, blob.data.data(), len).is_ok()) return Step::Fault;
            out->in.push_back(std::move(blob));
            break;
        }
        case ArgKind::IovIn:
        case ArgKind::IovOut: {
            uint64_t cnt = out->args[a.len];
            if (cnt > kIovMax) return Step::Fault;
            if (!merge(need_range(b, ptr, cnt * sizeof(KIovec)))) break;
            std::vector<KIovec> iov(cnt);
            if (cnt && !mem_read(b.mem.get(), ptr, iov.data(), cnt * sizeof(KIovec)).is_ok()) return Step::Fault;
            uint64_t total = 0;
            for (const KIovec& v : iov) {
                uint64_t take = std::min(v.len, kMaxIo - total);
                if (a.kind == ArgKind::IovIn) {
                    if (!merge(need_range(b, v.base, take))) continue;
                    size_t off = blob.data.size();
                    blob.data.resize(off + take);
                    if (take && !mem_read(b.mem.get(), v.base, blob.data.data() + off, take).is_ok()) return Step::Fault;
                } else if (!range_writable(b, v.base, take)) {
                    return Step::Fault;
                }
                total += take;
            }
            if (a.kind == ArgKind::IovOut) { // tell the master how much room there is
                blob.data.resize(8);
                memcpy(blob.data.data(), &total, 8);
            }
            if (result == Step::Done) out->in.push_back(std::move(blob));
            break;
        }
        case ArgKind::Out:
        case ArgKind::OutAll: {
            uint64_t cap = out->args[a.len];
            if (transfers_bytes(nr) && cap > kMaxIo) {
                cap = kMaxIo;
                out->args[a.len] = cap;
            }
            if (cap > kMaxIo || !range_writable(b, ptr, cap)) return Step::Fault;
            break;
        }
        case ArgKind::SOut:
            if (!range_writable(b, ptr, a.size)) return Step::Fault;
            break;
        default:
            break;
        }
    }
    return result;
}

void BodyRunner::continue_syscall(Body& b) {
    {
        long ret = 0;
        Step s = Step::Done;
        if (local_time(b, &ret, &s)) {
            if (s == Step::Wait) {
                b.st = St::AwaitPages; // the result's page is on its way; then again from the top
                return;
            }
            return finish_syscall(b, s == Step::Fault ? -EFAULT : ret);
        }
    }
    if (b.desc->cls == SysClass::Local) {
        long ret = 0;
        Step s = local_syscall(b, &ret);
        if (s == Step::Wait) {
            b.st = St::AwaitPages;
            return;
        }
        return finish_syscall(b, s == Step::Fault ? -EFAULT : ret);
    }
    XSyscallMsg msg;
    Step s = gather(b, &msg);
    if (s == Step::Wait) {
        b.st = St::AwaitPages; // retried from scratch when the pages arrive
        return;
    }
    if (s == Step::Fault) return finish_syscall(b, -EFAULT);
    msg.seq = ++b.seq;
    b.sys_t0 = now_ns();
    b.st = St::AwaitReply;
    metrics_.add("body_syscalls_forwarded");
    to_master_(make_msg(MsgType::XSyscall, msg));
}

BodyRunner::Step BodyRunner::write_out(Body& b, uint64_t addr, const uint8_t* data, size_t len) {
    Step result = Step::Done;
    for (size_t off = 0; off < len;) {
        uint64_t at = addr + off, page = pg(at);
        size_t chunk = size_t(std::min<uint64_t>(page + kPg - at, len - off));
        auto it = b.pages.find(page);
        if (it != b.pages.end()) {
            if (it->second != 2) mark_dirty(b, page);
            if (!mem_write(b.mem.get(), at, data + off, chunk).is_ok()) return Step::Fault;
        } else if (chunk == kPg) {
            // The whole page is being replaced: no need to fetch the old one.
            uffdio_copy cp{};
            cp.dst = page;
            cp.src = reinterpret_cast<uint64_t>(data + off);
            cp.len = kPg;
            if (ioctl(b.uffd.get(), UFFDIO_COPY, &cp) != 0 && errno != EEXIST) return Step::Fault;
            b.pages[page] = 2;
        } else {
            if (!find_vma(b, page)) return Step::Fault;
            request_page(b, page); // need the rest of the page first
            result = Step::Wait;
        }
        off += chunk;
    }
    return result;
}

void BodyRunner::apply_reply(Body& b) {
    XSysRetMsg& r = b.reply;
    // Address-space changes first, exactly once.
    for (const XVma& v : r.unmap) {
        long ret = 0;
        (void)inject(b, SYS_munmap, v.start, v.end - v.start, 0, 0, 0, 0, &ret);
        vma_remove(b, v.start, v.end);
    }
    for (const XVma& v : r.map) {
        Status st = map_vma(b, v);
        if (!st.is_ok()) return go_home(b, XReason::Error, -1, st.str());
    }
    for (const XVma& v : r.protect) {
        long ret = 0;
        (void)inject(b, SYS_mprotect, v.start, v.end - v.start, v.prot, 0, 0, 0, &ret);
        // Keep our own record of protections in step (contents are untouched).
        std::vector<XVma> pieces;
        for (auto it = b.vmas.begin(); it != b.vmas.end(); ++it)
            if (it->second.start < v.end && v.start < it->second.end) pieces.push_back(it->second);
        for (const XVma& p : pieces) {
            b.vmas.erase(p.start);
            if (p.start < v.start) b.vmas[p.start] = XVma{p.start, v.start, p.prot};
            if (p.end > v.end) b.vmas[v.end] = XVma{v.end, p.end, p.prot};
            uint64_t s = std::max(p.start, v.start), e = std::min(p.end, v.end);
            b.vmas[s] = XVma{s, e, v.prot};
        }
    }
    r.unmap.clear();
    r.map.clear();
    r.protect.clear();

    Step result = Step::Done;
    for (const XBlob& blob : r.out) {
        if (blob.arg >= 6) continue;
        const ArgDesc& a = b.desc->a[blob.arg];
        uint64_t ptr = arg_of(b.sregs, blob.arg);
        if (!ptr || !carries_out(a.kind)) continue;
        Step s = Step::Done;
        if (a.kind == ArgKind::IovOut) {
            uint64_t cnt = arg_of(b.sregs, a.len);
            std::vector<KIovec> iov(cnt);
            if (cnt && !mem_read(b.mem.get(), ptr, iov.data(), cnt * sizeof(KIovec)).is_ok()) s = Step::Fault;
            size_t off = 0;
            for (size_t k = 0; k < iov.size() && off < blob.data.size() && s != Step::Fault; ++k) {
                size_t take = size_t(std::min<uint64_t>(iov[k].len, blob.data.size() - off));
                Step t = write_out(b, iov[k].base, blob.data.data() + off, take);
                if (t != Step::Done) s = t;
                off += take;
            }
        } else {
            s = write_out(b, ptr, blob.data.data(), blob.data.size());
        }
        if (s == Step::Fault) {
            b.have_reply = false;
            return finish_syscall(b, -EFAULT);
        }
        if (s == Step::Wait) result = Step::Wait;
    }
    if (result == Step::Wait) {
        b.st = St::AwaitPages; // writes are idempotent; retried when the pages arrive
        return;
    }
    b.have_reply = false;
    finish_syscall(b, long(r.ret));
}

void BodyRunner::finish_syscall(Body& b, long ret) {
    user_regs_struct r = b.sregs;
    r.rax = uint64_t(ret);
    r.rcx = r.rip;     // what the SYSCALL instruction leaves behind
    r.r11 = r.eflags;
    r.orig_rax = uint64_t(-1);
    if (!pt_setregs(b.pid, r).is_ok()) return;
    resume(b);
    metrics_.observe("body_stop_service_ns", now_ns() - b.stop_t0);
}

// ============================ messages from the master ============================

void BodyRunner::handle(Msg&& m) {
    switch (m.type) {
    case MsgType::XBegin: {
        XBeginMsg x;
        if (parse_msg(m, &x)) begin(x);
        return;
    }
    case MsgType::XSysRet: {
        XSysRetMsg r;
        if (!parse_msg(m, &r)) return;
        Body* b = find(r.xid);
        if (!b || b->st != St::AwaitReply || r.seq != b->seq) return;
        b->reply = std::move(r);
        b->have_reply = true;
        metrics_.observe("body_syscall_roundtrip_ns", now_ns() - b->sys_t0);
        return apply_reply(*b);
    }
    case MsgType::XPage: {
        XPageView p; // the page data stays in the message
        if (!parse_xpage(m, &p)) return;
        if (Body* b = find(p.xid)) on_page(*b, p);
        return;
    }
    case MsgType::XStop: {
        XSimpleMsg s;
        if (!parse_msg(m, &s)) return;
        if (Body* b = find(s.xid)) go_home(*b, XReason::Requested, -1, s.text);
        return;
    }
    case MsgType::XKill: {
        XSimpleMsg s;
        if (parse_msg(m, &s)) destroy(s.xid);
        return;
    }
    default:
        return;
    }
}

// ============================ worker threads ============================

void BodyShards::compare_clocks() {
    XClockMsg c;
    c.slave = read_xclocks();
    to_master_(make_msg(MsgType::XClockReq, c));
}

BodyShards::BodyShards(Reactor& main, std::string stub_path, std::function<void(Msg)> to_master, unsigned workers)
    : main_(main), to_master_(to_master) {
    clock_timer_ = main_.add_timer(1000000000ull, [this] { compare_clocks(); }, 1000000000ull);
    if (workers == 0) workers = 1;
    for (unsigned i = 0; i < workers; ++i) {
        auto sh = std::make_unique<Shard>();
        Shard* s = sh.get();
        // Messages to the master leave through the link, which the main thread owns.
        s->runner = std::make_unique<BodyRunner>(s->reactor, s->metrics, stub_path, [&main, to_master](Msg m) {
            main.post([to_master, m = std::move(m)]() mutable { to_master(std::move(m)); });
        }, clocks_);
        s->reactor.add_timer(1000000000ull, [s] {
            std::lock_guard<std::mutex> lk(s->pub_mu);
            s->published = s->metrics;
        }, 1000000000ull);
        s->reactor.set_slow_hook(5000000, [i](const char* kind, int fd, uint64_t ns) {
            LOG_WARN("body worker %u stalled %.1f ms in a %s (fd %d)", i, double(ns) / 1e6, kind, fd);
        });
        s->thread = std::thread([s] {
            raise_priority(); // not inherited from the main thread
            s->reactor.run();
        });
        shards_.push_back(std::move(sh));
    }
}

BodyShards::~BodyShards() {
    main_.cancel_timer(clock_timer_);
    kill_all();
    for (auto& s : shards_) {
        Shard* sp = s.get();
        s->reactor.post([sp] {
            sp->runner.reset(); // on its own thread: it is the tracer of its helper process
            sp->reactor.stop();
        });
        if (s->thread.joinable()) s->thread.join();
    }
}

void BodyShards::handle(Msg&& m) {
    if (m.type == MsgType::XClock) {
        XClocks received = read_xclocks(); // before anything else: delay here is measurement error
        XClockMsg c;
        if (parse_msg(m, &c)) clocks_->add(c.slave, c.master, received);
        return;
    }
    if (m.type == MsgType::XBegin && clocks_->empty()) compare_clocks(); // do not wait for the timer
    // Every export message starts with the export id.
    if (m.payload.size() < 8) return;
    uint64_t xid;
    memcpy(&xid, m.payload.data(), 8);
    Shard* s = shards_[xid % shards_.size()].get();
    s->reactor.post([s, m = std::move(m)]() mutable { s->runner->handle(std::move(m)); });
}

void BodyShards::on_sigchld() {
    uint64_t t0 = now_ns();
    for (auto& sh : shards_) {
        Shard* s = sh.get();
        s->reactor.post([s, t0] {
            s->metrics.observe("body_sigchld_dispatch_ns", now_ns() - t0);
            s->runner->on_sigchld();
        });
    }
}

void BodyShards::kill_all() {
    std::mutex mu;
    std::condition_variable cv;
    size_t left = shards_.size();
    for (auto& sh : shards_) {
        Shard* s = sh.get();
        s->reactor.post([s, &mu, &cv, &left] {
            s->runner->kill_all();
            std::lock_guard<std::mutex> lk(mu);
            --left;
            cv.notify_one();
        });
    }
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return left == 0; });
}

uint32_t BodyShards::count() const {
    uint32_t n = 0;
    for (const auto& s : shards_) n += s->runner->count();
    return n;
}

std::vector<std::pair<uint64_t, pid_t>> BodyShards::pids() const {
    std::vector<std::pair<uint64_t, pid_t>> out;
    for (const auto& s : shards_) s->runner->pids(&out);
    return out;
}

void BodyShards::merge_metrics(Metrics* into) {
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lk(s->pub_mu);
        into->merge(s->published);
    }
}

} // namespace dnuma
