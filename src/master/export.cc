// SPDX-License-Identifier: GPL-2.0-only
#include "master/export.h"

#include <algorithm>
#include <cinttypes>
#include <csignal>
#include <cstring>
#include <iterator>

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/log.h"

namespace dnuma {

namespace {
constexpr uint64_t kPg = 4096;
// The data area must hold one call's largest input and output. Transfers are
// limited to 8 MiB per call on the slave side, so 32 MiB always suffices.
constexpr uint64_t kScratchBytes = (32ull << 20) + kPg;
constexpr long kErestartSys = -512, kErestartNoIntr = -513, kErestartNoHand = -514, kErestartBlock = -516;
inline uint64_t pgup(uint64_t a) { return (a + kPg - 1) & ~(kPg - 1); }
inline uint64_t al8(uint64_t a) { return (a + 7) & ~7ull; }

struct KIovec {
    uint64_t base, len;
};

// Replaces the vDSO's time functions with plain system calls. The real ones
// read kernel-maintained data pages that do not exist on the slave; a system
// call is forwarded home like any other.
void patch_vdso(std::vector<uint8_t>* img) {
    if (img->size() < sizeof(Elf64_Ehdr)) return;
    const Elf64_Ehdr* eh = reinterpret_cast<const Elf64_Ehdr*>(img->data());
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_shentsize != sizeof(Elf64_Shdr)) return;
    if (eh->e_shoff + uint64_t(eh->e_shnum) * sizeof(Elf64_Shdr) > img->size()) return;
    const Elf64_Shdr* sh = reinterpret_cast<const Elf64_Shdr*>(img->data() + eh->e_shoff);
    static const struct {
        const char* name;
        uint32_t nr;
    } kFns[] = {{"clock_gettime", SYS_clock_gettime}, {"gettimeofday", SYS_gettimeofday}, {"time", SYS_time},
                {"clock_getres", SYS_clock_getres},   {"getcpu", SYS_getcpu},             {"getrandom", SYS_getrandom}};
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf64_Shdr& str = sh[sh[i].sh_link];
        if (sh[i].sh_offset + sh[i].sh_size > img->size() || str.sh_offset + str.sh_size > img->size()) continue;
        size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t k = 0; k < n; ++k) {
            Elf64_Sym sym;
            memcpy(&sym, img->data() + sh[i].sh_offset + k * sizeof sym, sizeof sym);
            if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC || sym.st_name >= str.sh_size) continue;
            const char* name = reinterpret_cast<const char*>(img->data() + str.sh_offset + sym.st_name);
            size_t max = str.sh_size - sym.st_name;
            if (!memchr(name, 0, max)) continue;
            if (strncmp(name, "__vdso_", 7) == 0) name += 7;
            for (const auto& f : kFns) {
                if (strcmp(name, f.name) != 0 || sym.st_value + 8 > img->size()) continue;
                uint8_t code[8] = {0xb8, 0, 0, 0, 0, 0x0f, 0x05, 0xc3}; // mov $nr,%eax; syscall; ret
                memcpy(code + 1, &f.nr, 4);
                memcpy(img->data() + sym.st_value, code, sizeof code);
            }
        }
    }
}

uint64_t find_syscall_insn(int memfd, const std::vector<ProcVma>& maps) {
    // Prefer the vDSO (always present and executable); otherwise any code.
    for (int pass = 0; pass < 2; ++pass) {
        for (const ProcVma& v : maps) {
            if (!(v.prot & kVmExec) || !(v.prot & kVmRead)) continue;
            if (pass == 0 && v.name != "[vdso]") continue;
            uint64_t len = std::min<uint64_t>(v.end - v.start, 1u << 20);
            std::vector<uint8_t> buf(len);
            if (!mem_read(memfd, v.start, buf.data(), len).is_ok()) continue;
            for (uint64_t i = 0; i + 1 < len; ++i)
                if (buf[i] == 0x0f && buf[i + 1] == 0x05) return v.start + i;
        }
    }
    return 0;
}

std::string comm_of(pid_t pid) { return read_attr("/proc/" + std::to_string(pid) + "/comm"); }

// Read-only VMA permissions constrain this process. Other mappings and file
// descriptors can still modify the same backing. These seals are permanent.
bool immutable_file(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) < 0 || !S_ISREG(st.st_mode)) return false;
    Fd fd(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY));
    if (!fd.valid()) return false;
    int seals = fcntl(fd.get(), F_GET_SEALS);
    constexpr int required = F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW;
    return seals >= 0 && (seals & required) == required;
}

// A restricted Linux affinity mask is a placement decision, including a
// cpuset restriction. The automatic balancer must leave that task at home.
bool unrestricted_home_affinity(const std::string& status) {
    const std::string field = "\nCpus_allowed_list:";
    size_t pos = status.find(field);
    if (pos == std::string::npos) return false;
    pos += field.size();
    auto allowed = parse_cpulist(status.substr(pos, status.find('\n', pos) - pos));
    auto online = parse_cpulist(read_attr("/sys/devices/system/cpu/online"));
    auto possible = parse_cpulist(read_attr("/sys/devices/system/cpu/possible"));
    // The optional kernel view appends remote CPUs to online only.
    std::vector<int> local;
    std::set_intersection(online.begin(), online.end(), possible.begin(), possible.end(), std::back_inserter(local));
    return !local.empty() && std::includes(allowed.begin(), allowed.end(), local.begin(), local.end());
}

int thread_count(pid_t pid) {
    int n = 0;
    DIR* d = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (!d) return -1;
    while (dirent* e = readdir(d)) n += e->d_name[0] != '.';
    closedir(d);
    return n;
}
} // namespace

uint64_t proc_start_time(pid_t pid) {
    std::string s = read_attr("/proc/" + std::to_string(pid) + "/stat");
    size_t paren = s.rfind(')');
    if (paren == std::string::npos) return 0;
    std::vector<std::string> f = split(s.substr(paren + 2), ' ');
    return f.size() > 19 ? strtoull(f[19].c_str(), nullptr, 10) : 0;
}

ExportManager::ExportManager(Reactor& reactor, Metrics& metrics, Hooks hooks, WorkerPool& pool, uint64_t xid_first, uint64_t xid_step)
    : reactor_(reactor), metrics_(metrics), hooks_(std::move(hooks)), next_xid_(xid_first), xid_step_(xid_step), pool_(pool) {}

ExportManager::~ExportManager() {
    *alive_ = false;
    // Nothing may be left half-exported: without us the bodies are orphans.
    std::vector<uint64_t> ids;
    for (auto& [id, x] : exports_) ids.push_back(id);
    for (uint64_t id : ids)
        if (Export* x = find(id)) lost(*x, "master shutting down");
}

ExportManager::Export* ExportManager::find(uint64_t xid) {
    auto it = exports_.find(xid);
    return it == exports_.end() ? nullptr : &it->second;
}

size_t ExportManager::count(MachineId machine) const {
    std::lock_guard<std::mutex> lk(meta_mu_);
    size_t n = 0;
    for (const auto& [id, x] : exports_) n += x.machine == machine; // never changes while exported
    return n;
}

size_t ExportManager::count() const {
    std::lock_guard<std::mutex> lk(meta_mu_);
    return exports_.size();
}

bool ExportManager::is_exported(pid_t pid) const {
    std::lock_guard<std::mutex> lk(meta_mu_);
    return by_pid_.count(pid) != 0;
}

bool ExportManager::is_pinned(pid_t pid) const {
    uint64_t started = 0;
    {
        std::lock_guard<std::mutex> lk(meta_mu_);
        auto it = pinned_.find(pid);
        if (it == pinned_.end()) return false;
        started = it->second;
    }
    return started == proc_start_time(pid);
}

std::vector<ExportManager::Info> ExportManager::list() const {
    static const char* names[] = {"starting", "running", "running", "in-syscall", "running", "coming-home"};
    std::vector<Info> out;
    uint64_t now = now_ns();
    for (const auto& [id, x] : exports_)
        out.push_back({id, x.pid, x.comm, names[int(x.st)], x.uid, x.machine, x.cpu, x.syscalls, x.pages_out, (now - x.t0) / 1000000});
    return out;
}

std::vector<std::string> ExportManager::describe(MachineId machine) const {
    std::vector<std::string> out;
    for (const auto& [id, x] : exports_)
        if (x.machine == machine) out.push_back(format("PID %d  %s  (exported native process)", x.pid, x.comm.c_str()));
    return out;
}

void ExportManager::erase(uint64_t xid) {
    auto it = exports_.find(xid);
    if (it == exports_.end()) return;
    MachineId m = it->second.machine;
    pid_t pid = it->second.pid;
    reactor_.cancel_timer(it->second.idle_timer);
    {
        std::lock_guard<std::mutex> lk(meta_mu_);
        by_pid_.erase(it->second.pid);
        exports_.erase(it);
    }
    if (hooks_.on_end) hooks_.on_end(xid, m, pid);
}

// ============================ starting ============================

Status ExportManager::bootstrap(Export& x, std::vector<XVma>* vmas, std::vector<uint8_t>* xstate) {
    pid_t pid = x.pid;
    user_regs_struct r;
    DNUMA_TRY(pt_getregs(pid, &r));
    if (r.cs != 0x33) return Status::err(ENOTSUP, "not a 64-bit process");
    long orig = long(r.orig_rax), ax = long(r.rax);
    if (orig >= 0) {
        // Interrupted inside a system call. Undo it the way the kernel would
        // before running a handler, so the body simply issues it again.
        if (ax == kErestartBlock) return Status::err(EBUSY, "in a system call that cannot be restarted remotely");
        if (ax == kErestartSys || ax == kErestartNoIntr || ax == kErestartNoHand) {
            r.rax = r.orig_rax;
            r.rip -= 2;
        }
        r.orig_rax = uint64_t(-1);
        DNUMA_TRY(pt_setregs(pid, r));
    }
    x.orig_regs = r;
    DNUMA_TRY(pt_get_xstate(pid, xstate));

    std::vector<ProcVma> maps;
    DNUMA_TRY(read_proc_maps(pid, &maps));
    for (const ProcVma& v : maps) {
        // A writable shared mapping is visible to other processes (or a file)
        // as it changes; a body's private copy could not honour that.
        if (v.shared && (v.prot & kVmWrite)) return Status::err(ENOTSUP, "has a writable shared mapping (%s)", v.name.empty() ? "anonymous" : v.name.c_str());
        if (v.shared && !immutable_file(format("/proc/%d/map_files/%" PRIx64 "-%" PRIx64, pid, v.start, v.end)))
            return Status::err(ENOTSUP, "shared mapping at 0x%" PRIx64 " can change through another mapping or descriptor", v.start);
        if (v.name == "[vdso]") {
            x.vdso_start = v.start;
            x.vdso_end = v.end;
        }
    }
    x.mem.reset(open(("/proc/" + std::to_string(pid) + "/mem").c_str(), O_RDWR | O_CLOEXEC));
    x.mem_shared = std::make_shared<Fd>(open(("/proc/" + std::to_string(pid) + "/mem").c_str(), O_RDONLY | O_CLOEXEC));
    if (!x.mem.valid()) return Status::sys("open /proc/%d/mem", pid);
    x.sys_insn = find_syscall_insn(x.mem.get(), maps);
    if (!x.sys_insn) return Status::err(ENOTSUP, "no system-call instruction found to drive the process with");

    // Our working area inside P: one executable page holding "syscall; int3"
    // and a data area for system-call arguments.
    uint64_t a[6] = {0, kScratchBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, uint64_t(-1), 0};
    long ret = 0;
    DNUMA_TRY(pt_inject(pid, x.sys_insn, SYS_mmap, a, &ret));
    if (ret < 0 && ret > -4096) return Status::err(int(-ret), "cannot map the working area in the process");
    x.scratch = uint64_t(ret);
    x.gadget = x.scratch;
    const uint8_t gadget[3] = {0x0f, 0x05, 0xcc};
    DNUMA_TRY(mem_write(x.mem.get(), x.gadget, gadget, sizeof gadget));
    uint64_t p[6] = {x.scratch, kPg, PROT_READ | PROT_EXEC, 0, 0, 0};
    DNUMA_TRY(pt_inject(pid, x.sys_insn, SYS_mprotect, p, &ret));
    if (ret != 0) return Status::err(int(-ret), "cannot make the gadget page executable");
    uint64_t b[6] = {0, 0, 0, 0, 0, 0};
    DNUMA_TRY(pt_inject(pid, x.sys_insn, SYS_brk, b, &ret));
    x.brk = uint64_t(ret);

    if (x.vdso_end > x.vdso_start) {
        x.vdso.resize(x.vdso_end - x.vdso_start);
        if (mem_read(x.mem.get(), x.vdso_start, x.vdso.data(), x.vdso.size()).is_ok()) patch_vdso(&x.vdso);
        else x.vdso.clear();
        x.vdso_shared = std::make_shared<const std::vector<uint8_t>>(x.vdso);
    }
    for (const ProcVma& v : maps) {
        // Kernel data pages have no meaning on another machine.
        if (v.name == "[vvar]" || v.name == "[vvar_vclock]" || v.name == "[vsyscall]") continue;
        vmas->push_back(XVma{v.start, v.end, v.prot});
    }
    return Status::ok();
}

Status ExportManager::start(pid_t pid, MachineId machine, int cpu, int node, int local_cpu, ExportPlacement placement) {
    if (by_pid_.count(pid)) return Status::err(EALREADY, "already exported");
    if (thread_count(pid) != 1) return Status::err(ENOTSUP, "has more than one thread");
    auto proc_status = [&] { return read_attr("/proc/" + std::to_string(pid) + "/status"); };
    if (placement == ExportPlacement::Balance && !unrestricted_home_affinity(proc_status()))
        return Status::err(EAGAIN, "home CPU affinity is restricted");
    if (ptrace(PTRACE_SEIZE, pid, 0, PTRACE_O_TRACESYSGOOD) != 0) return Status::sys("cannot take control of pid %d", pid);
    int st = 0;
    Status s = pt_interrupt(pid, &st);
    if (!s.is_ok()) return s;
    if (!is_event_stop(st) || stop_signal(st) != SIGTRAP) {
        // A signal (or job-control stop) got there first; leave it to that.
        int sig = is_event_stop(st) ? 0 : stop_signal(st);
        ptrace(PTRACE_DETACH, pid, 0, sig);
        return Status::err(EAGAIN, "a signal is being delivered");
    }
    // A clone can finish between the initial task census and the stop.
    // Check again before treating the home address space as quiescent.
    if (thread_count(pid) != 1) {
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return Status::err(ENOTSUP, "thread group changed during export");
    }
    std::string status = proc_status();
    if (placement == ExportPlacement::Balance && !unrestricted_home_affinity(status)) {
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return Status::err(EAGAIN, "home CPU affinity changed during export");
    }
    Export x;
    x.xid = next_xid_;
    next_xid_ += xid_step_;
    x.pid = pid;
    x.comm = comm_of(pid);
    x.machine = machine;
    x.cpu = cpu;
    x.explicit_cpu = placement == ExportPlacement::SelectCpu;
    x.t0 = now_ns();
    x.stopped = true;
    size_t up = status.find("\nUid:"), gp = status.find("\nGid:");
    if (up != std::string::npos) x.uid = uint32_t(strtoul(split(status.substr(up + 5, 64), '\t')[1].c_str(), nullptr, 10)); // effective
    if (gp != std::string::npos) x.gid = uint32_t(strtoul(split(status.substr(gp + 5, 64), '\t')[1].c_str(), nullptr, 10));

    XBeginMsg b;
    s = bootstrap(x, &b.vmas, &b.xstate);
    if (!s.is_ok()) {
        if (x.scratch) {
            uint64_t a[6] = {x.scratch, kScratchBytes, 0, 0, 0, 0};
            long ret;
            (void)pt_inject(pid, x.sys_insn, SYS_munmap, a, &ret);
        }
        (void)pt_setregs(pid, x.orig_regs.cs ? x.orig_regs : user_regs_struct{});
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return s;
    }
    b.xid = x.xid;
    b.home_pid = pid;
    b.uid = x.uid;
    b.gid = x.gid;
    b.regs = regs_to_bytes(x.orig_regs);
    b.cpu = cpu;
    b.node = node;
    b.local_cpu = local_cpu;
    b.clocks = read_xclocks(); // P is stopped: it has seen nothing later than this
    {
        // Where the kernel publishes the current CPU number to this process.
        struct {
            uint64_t rseq_abi_pointer;
            uint32_t rseq_abi_size, signature, flags, pad;
        } rc{};
        if (ptrace(static_cast<__ptrace_request>(0x420f) /* PTRACE_GET_RSEQ_CONFIGURATION */, pid, sizeof rc, &rc) == long(sizeof rc))
            b.rseq = rc.rseq_abi_pointer;
    }
    // While exported, P computes nothing here: it only executes short kernel
    // operations for its body. Those must not queue behind this machine's
    // CPU-bound work, or every forwarded call would wait a scheduler slice.
    raise_priority_of(pid, &x.home_sched);
    uint64_t xid = x.xid;
    {
        std::lock_guard<std::mutex> lk(meta_mu_);
        by_pid_[pid] = xid;
        exports_.emplace(xid, std::move(x));
    }
    hooks_.send(machine, make_msg(MsgType::XBegin, b));
    metrics_.add("exports_started");
    return Status::ok();
}

void ExportManager::abort_start(Export& x, const std::string& why) {
    LOG_WARN("export of pid %d (%s) to %s failed: %s; it continues at home", x.pid, x.comm.c_str(),
             hooks_.machine_name(x.machine).c_str(), why.c_str());
    uint64_t a[6] = {x.scratch, kScratchBytes, 0, 0, 0, 0};
    long ret;
    (void)pt_inject(x.pid, x.sys_insn, SYS_munmap, a, &ret);
    (void)pt_setregs(x.pid, x.orig_regs);
    restore_priority_of(x.pid, x.home_sched);
    ptrace(PTRACE_DETACH, x.pid, 0, 0);
    metrics_.add("exports_failed");
    {
        uint64_t started = proc_start_time(x.pid);
        std::lock_guard<std::mutex> lk(meta_mu_);
        pinned_[x.pid] = started;
    }
    erase(x.xid);
}

// ============================ P's state machine ============================

void ExportManager::park(Export& x) {
    // P waits in ppoll(NULL, 0, NULL, NULL): asleep, but a signal sent to it
    // interrupts the wait and is reported to us at once.
    user_regs_struct r = x.orig_regs;
    r.rip = x.gadget;
    r.rax = SYS_ppoll;
    r.orig_rax = uint64_t(-1);
    r.rdi = r.rsi = r.rdx = r.r10 = r.r8 = r.r9 = 0;
    (void)pt_setregs(x.pid, r);
    ptrace(PTRACE_CONT, x.pid, 0, 0);
    x.st = St::Parked;
    x.stopped = false;
}

bool ExportManager::ignorable(pid_t pid, int sig) {
    // Would the kernel have discarded this signal for an untraced process?
    std::string status;
    if (!read_file("/proc/" + std::to_string(pid) + "/status", &status).is_ok()) return false;
    auto mask = [&](const char* key) -> uint64_t {
        size_t p = status.find(key);
        return p == std::string::npos ? 0 : strtoull(status.c_str() + p + strlen(key), nullptr, 16);
    };
    uint64_t bit = 1ull << (sig - 1);
    if (mask("\nSigCgt:") & bit) return false;
    if (mask("\nSigIgn:") & bit) return true;
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT;
}

// Stops a parked P. Returns false if P is gone. A real signal that arrives
// first is recorded in pending_sig (P is stopped in that case too).
bool ExportManager::stop_parked(Export& x) {
    if (x.stopped) return true;
    if (ptrace(PTRACE_INTERRUPT, x.pid, 0, 0) != 0) return false;
    for (;;) {
        int st = 0;
        if (!pt_wait_stop(x.pid, &st).is_ok()) return false;
        if (is_event_stop(st)) {
            if (stop_signal(st) != SIGTRAP) x.group_stop = true; // job-control stop
            break;
        }
        int sig = stop_signal(st);
        if (ignorable(x.pid, sig)) {
            ptrace(PTRACE_CONT, x.pid, 0, 0);
            continue;
        }
        x.pending_sig = sig;
        break;
    }
    x.stopped = true;
    return true;
}

void ExportManager::on_sigchld() {
    // The only processes that report to this thread are the ones it traces
    // (__WNOTHREAD: not those of the other workers), so "any" is exactly the
    // set of processes exported through this manager.
    for (int budget = 256; budget > 0; --budget) {
        int st = 0;
        pid_t pid = waitpid(-1, &st, WNOHANG | __WALL | __WNOTHREAD);
        if (pid <= 0) return;
        auto it = by_pid_.find(pid);
        if (it == by_pid_.end()) continue;
        if (Export* x = find(it->second)) handle_wait(*x, st);
    }
    reactor_.post([this] { on_sigchld(); }); // more than a batch: let others run first
}

void ExportManager::handle_wait(Export& x, int status) {
    if (!WIFSTOPPED(status)) {
        // P itself is gone (killed, or exited through us). Its body goes too.
        LOG_INFO("exported pid %d (%s) ended at home", x.pid, x.comm.c_str());
        XSimpleMsg k;
        k.xid = x.xid;
        hooks_.send(x.machine, make_msg(MsgType::XKill, k));
        metrics_.add("exports_ended");
        return erase(x.xid);
    }
    int sig = stop_signal(status);
    bool event = is_event_stop(status);
    if (x.st == St::InSyscall && !event && sig == SIGTRAP) {
        user_regs_struct r;
        if (pt_getregs(x.pid, &r).is_ok() && r.rip == x.gadget + 3) {
            x.stopped = true;
            return complete_syscall(x);
        }
    }
    if (event && sig == SIGTRAP) {
        if (x.st == St::Stopping) { // stopped as requested: run the call that was waiting
            x.stopped = true;
            XSyscallMsg m = std::move(x.pending);
            return run_syscall(x, m);
        }
        ptrace(PTRACE_CONT, x.pid, 0, 0); // a leftover interrupt request
        return;
    }
    if (!event && ignorable(x.pid, sig)) {
        // Discard it; the kernel restarts the interrupted wait or call itself.
        ptrace(PTRACE_CONT, x.pid, 0, 0);
        return;
    }
    // A signal P must really receive (or a job-control stop). Its user state
    // has to be at home for that.
    x.stopped = true;
    x.group_stop = event;
    x.pending_sig = event ? 0 : sig;
    // Did the signal catch P inside the call it is executing for its body?
    // Not if P has not reached the call instruction yet (the signal was
    // already waiting when P was set going): then nothing was executed, and
    // the call is simply issued again at home, like any call that was never
    // forwarded.
    x.mid_syscall = false;
    if (x.st == St::InSyscall) {
        user_regs_struct r;
        x.mid_syscall = !pt_getregs(x.pid, &r).is_ok() || r.rip != x.gadget;
    }
    if (x.st != St::Homing) start_home(x, format("signal %d", sig));
}

// ============================ system calls ============================

// After a call P stays stopped for a moment: programs tend to make system
// calls in bursts, and a stopped P can start the next one immediately. If
// none comes, P is parked again so that signals are noticed promptly.
void ExportManager::idle(Export& x) {
    x.st = St::Idle;
    x.stopped = true;
    uint64_t xid = x.xid;
    x.idle_timer = reactor_.add_timer(1000000, [this, xid] {
        Export* x = find(xid);
        if (x && x->st == St::Idle) park(*x);
    });
}

void ExportManager::on_syscall(Export& x, const XSyscallMsg& m) {
    x.sys_t0 = now_ns();
    if (x.st == St::Idle) {
        reactor_.cancel_timer(x.idle_timer);
        return run_syscall(x, m);
    }
    if (x.st != St::Parked) return; // already on its way home; the call was not executed
    // Ask P to stop and carry on when it reports that it has (handle_wait):
    // the daemon never waits for another process here.
    x.pending = m;
    x.st = St::Stopping;
    if (ptrace(PTRACE_INTERRUPT, x.pid, 0, 0) != 0) return lost(x, "process vanished");
}

void ExportManager::run_syscall(Export& x, const XSyscallMsg& m) {
    metrics_.observe("export_stop_parked_ns", now_ns() - x.sys_t0);

    uint64_t args[6];
    for (int i = 0; i < 6; ++i) args[i] = m.args[size_t(i)];
    if (m.nr == SYS_mmap && (args[3] & MAP_SHARED) &&
        ((args[2] & PROT_WRITE) || (args[3] & MAP_ANONYMOUS) ||
         !immutable_file(format("/proc/%d/fd/%d", x.pid, int(args[4]))))) {
        // Recall before mmap takes effect. The original thread executes it
        // once, including MAP_FIXED replacement or the kernel's error path.
        return start_home(x, "mapping mutable shared backing");
    }
    const SysDesc& d = describe_syscall(m.nr, args);
    x.desc = &d;
    x.nr = m.nr;
    x.seq = m.seq;
    x.outs.clear();
    memcpy(x.args, args, sizeof args);
    ++x.syscalls;

    auto reply_error = [&](long err) {
        XSysRetMsg r;
        r.xid = x.xid;
        r.seq = m.seq;
        r.ret = err;
        hooks_.send(x.machine, make_msg(MsgType::XSysRet, r));
        idle(x);
    };
    if (d.cls != SysClass::Forward && d.cls != SysClass::Memory) return reply_error(-ENOSYS);

    // Lay the pointer arguments out in the data area.
    uint64_t cursor = x.scratch + kPg, limit = x.scratch + kScratchBytes;
    auto take = [&](uint64_t n) -> uint64_t {
        uint64_t at = cursor;
        cursor = al8(cursor + n);
        return cursor <= limit ? at : 0;
    };
    auto blob_for = [&](int i) -> const XBlob* {
        for (const XBlob& b : m.in)
            if (b.arg == i) return &b;
        return nullptr;
    };
    for (int i = 0; i < 6; ++i) {
        const ArgDesc& a = d.a[i];
        if (!is_pointer(a.kind) || args[i] == 0) continue;
        const XBlob* in = blob_for(i);
        uint64_t at = 0;
        switch (a.kind) {
        case ArgKind::Str:
            if (!in || in->data.empty() || in->data.back() != 0) return reply_error(-ENAMETOOLONG);
            [[fallthrough]];
        case ArgKind::In:
        case ArgKind::SIn:
            if (!in) return reply_error(-EFAULT);
            if (!(at = take(in->data.size())) || !mem_write(x.mem.get(), at, in->data.data(), in->data.size()).is_ok()) return reply_error(-ENOMEM);
            args[i] = at;
            break;
        case ArgKind::SInOut:
        case ArgKind::PollFds:
            if (!in) return reply_error(-EFAULT);
            if (!(at = take(in->data.size())) || !mem_write(x.mem.get(), at, in->data.data(), in->data.size()).is_ok()) return reply_error(-ENOMEM);
            args[i] = at;
            x.outs.push_back({uint8_t(i), a.kind, at, in->data.size()});
            break;
        case ArgKind::IovIn:
        case ArgKind::IovOut: {
            if (!in) return reply_error(-EFAULT);
            uint64_t len = in->data.size();
            if (a.kind == ArgKind::IovOut) {
                if (in->data.size() != 8) return reply_error(-EFAULT);
                memcpy(&len, in->data.data(), 8);
            }
            uint64_t buf = take(len), iov = take(sizeof(KIovec));
            if (!buf || !iov) return reply_error(-ENOMEM);
            if (a.kind == ArgKind::IovIn && len && !mem_write(x.mem.get(), buf, in->data.data(), len).is_ok()) return reply_error(-ENOMEM);
            KIovec one{buf, len}; // the slave flattened (or will scatter) the vector
            if (!mem_write(x.mem.get(), iov, &one, sizeof one).is_ok()) return reply_error(-ENOMEM);
            args[i] = iov;
            args[a.len] = 1;
            if (a.kind == ArgKind::IovOut) x.outs.push_back({uint8_t(i), a.kind, buf, len});
            break;
        }
        case ArgKind::Out:
        case ArgKind::OutAll:
        case ArgKind::SOut: {
            uint64_t cap = a.kind == ArgKind::SOut ? a.size : args[a.len];
            if (!(at = take(cap))) return reply_error(-ENOMEM);
            args[i] = at;
            x.outs.push_back({uint8_t(i), a.kind, at, cap});
            break;
        }
        default:
            break;
        }
    }

    user_regs_struct r = x.orig_regs;
    r.rip = x.gadget;
    r.rax = uint64_t(m.nr);
    r.orig_rax = uint64_t(-1);
    r.rdi = args[0];
    r.rsi = args[1];
    r.rdx = args[2];
    r.r10 = args[3];
    r.r8 = args[4];
    r.r9 = args[5];
    if (!pt_setregs(x.pid, r).is_ok()) return lost(x, "cannot set registers");
    x.st = St::InSyscall;
    x.stopped = false;
    ptrace(PTRACE_CONT, x.pid, 0, 0); // P executes the call itself; we hear back via SIGCHLD
}

// Reads the call's results. With into_process, output data is stored through
// the original pointers in P's own memory (used when P is coming home).
long ExportManager::collect_results(Export& x, XSysRetMsg* ret, bool into_process) {
    user_regs_struct r;
    if (!pt_getregs(x.pid, &r).is_ok()) return -EIO;
    long rv = long(r.rax);
    bool ok = !(rv < 0 && rv > -4096);
    for (const OutPlan& o : x.outs) {
        uint64_t n = 0;
        switch (o.kind) {
        case ArgKind::Out:
        case ArgKind::IovOut: n = rv > 0 ? std::min<uint64_t>(uint64_t(rv), o.cap) : 0; break;
        default: n = ok ? o.cap : 0; break;
        }
        if (!n) continue;
        XBlob b;
        b.arg = o.arg;
        b.data.resize(n);
        if (!mem_read(x.mem.get(), o.addr, b.data.data(), n).is_ok()) continue;
        if (into_process && o.kind != ArgKind::IovOut) (void)mem_write(x.mem.get(), x.args[o.arg], b.data.data(), n);
        else ret->out.push_back(std::move(b));
    }
    return rv;
}

void ExportManager::complete_syscall(Export& x) {
    XSysRetMsg ret;
    ret.xid = x.xid;
    ret.seq = x.seq;
    long rv = collect_results(x, &ret, false);
    ret.ret = rv;
    bool ok = !(rv < 0 && rv > -4096);
    bool recall_after = false;

    if (x.desc->cls == SysClass::Memory && ok) {
        // Tell the body how its address space changed.
        auto mapped_in = [&](uint64_t start, uint64_t end) {
            std::vector<ProcVma> maps;
            if (!read_proc_maps(x.pid, &maps).is_ok()) return;
            for (const ProcVma& v : maps) {
                uint64_t s = std::max(v.start, start), e = std::min(v.end, end);
                if (s >= e) continue;
                if (v.shared && (v.prot & kVmWrite)) recall_after = true; // cannot be mirrored: finish at home
                ret.map.push_back(XVma{s, e, v.prot});
            }
        };
        uint64_t addr = x.args[0], len = pgup(x.args[1]);
        switch (x.nr) {
        case SYS_mmap:
            mapped_in(uint64_t(rv), uint64_t(rv) + len);
            break;
        case SYS_munmap:
            ret.unmap.push_back(XVma{addr, addr + len, 0});
            break;
        case SYS_mprotect:
            ret.protect.push_back(XVma{addr, addr + len, uint32_t(x.args[2] & 7)});
            break;
        case SYS_brk: {
            uint64_t now = uint64_t(rv), old = x.brk;
            if (pgup(now) > pgup(old)) mapped_in(pgup(old), pgup(now));
            else if (pgup(now) < pgup(old)) ret.unmap.push_back(XVma{pgup(now), pgup(old), 0});
            x.brk = now;
            break;
        }
        case SYS_madvise: // contents discarded: drop the body's copies, refetch on demand
            ret.unmap.push_back(XVma{addr, addr + len, 0});
            mapped_in(addr, addr + len);
            break;
        default:
            break;
        }
    }
    hooks_.send(x.machine, make_msg(MsgType::XSysRet, ret));
    metrics_.add("export_syscalls");
    idle(x);
    metrics_.observe("export_syscall_serve_ns", now_ns() - x.sys_t0);
    if (recall_after) {
        x.pin = true;
        start_home(x, "created a writable shared mapping");
    }
}

// ============================ memory ============================

// Reading the memory and building the reply is proportional to the amount of
// data, so it runs on a worker thread; this thread only hands the finished
// message to the link. Reading concurrently with P is safe: while its body
// waits for a page, P executes nothing that changes its memory.
void ExportManager::on_page_req(Export& x, const XPageMsg& rq) {
    uint64_t t_serve = now_ns();
    uint32_t want = std::min<uint32_t>(rq.count ? rq.count : 1, kXMaxPagesPerRequest);
    std::weak_ptr<bool> alive = alive_;
    pool_.submit([this, alive, t_serve, want, xid = x.xid, machine = x.machine, addr = rq.addr, mem = x.mem_shared,
                  vdso = x.vdso_shared, vdso_start = x.vdso_start, vdso_end = x.vdso_end]() mutable {
        Msg rep = xpage_begin(xid, addr, want);
        uint8_t* data = xpage_data(rep);
        // P's own view of its memory: anonymous pages, file-backed pages read
        // through the master's page cache, not-yet-touched pages as zeros.
        // The read stops early at the end of a mapping.
        uint32_t got = 0;
        while (mem && mem->valid() && got < want) {
            ssize_t n = pread(mem->get(), data + size_t(got) * kPg, size_t(want - got) * kPg, off_t(addr + uint64_t(got) * kPg));
            if (n < ssize_t(kPg)) break;
            got += uint32_t(size_t(n) / kPg);
        }
        // The vDSO is served as we patched it, not as the kernel provides it.
        for (uint32_t i = 0; i < got && vdso && !vdso->empty(); ++i) {
            uint64_t page = addr + uint64_t(i) * kPg;
            if (page >= vdso_start && page < vdso_end) memcpy(data + size_t(i) * kPg, vdso->data() + (page - vdso_start), kPg);
        }
        xpage_finish(&rep, got, got ? 0 : EFAULT);
        // The reply is moved, never copied, on its way to the link.
        auto boxed = std::make_shared<Msg>(std::move(rep));
        reactor_.post([this, alive, t_serve, got, xid, machine, boxed] {
            auto a = alive.lock();
            if (!a || !*a) return;
            Export* x = find(xid);
            if (!x || x->machine != machine) return; // ended meanwhile
            x->pages_out += got;
            metrics_.add("export_pages_out", got);
            hooks_.send(machine, std::move(*boxed));
            metrics_.observe("export_page_serve_ns", now_ns() - t_serve);
        });
    });
}


// ============================ coming home ============================

void ExportManager::start_home(Export& x, const std::string& why) {
    if (x.st == St::Homing) return;
    bool starting = x.st == St::Starting;
    reactor_.cancel_timer(x.idle_timer);
    x.st = St::Homing;
    x.why = why;
    if (starting) return; // decided when XReady arrives
    if (!x.have_home) {
        XSimpleMsg s;
        s.xid = x.xid;
        s.text = why;
        hooks_.send(x.machine, make_msg(MsgType::XStop, s));
    }
}

Status ExportManager::recall(pid_t pid, const std::string& why, bool pin) {
    auto it = by_pid_.find(pid);
    if (it == by_pid_.end()) return Status::err(ESRCH, "pid %d is not exported", pid);
    Export* x = find(it->second);
    x->pin = x->pin || pin;
    start_home(*x, why);
    return Status::ok();
}

void ExportManager::recall_all(MachineId machine, const std::string& why) {
    for (auto& [id, x] : exports_)
        if (x.machine == machine) start_home(x, why);
}

// The body read the clocks on the slave, corrected by the measured difference
// to ours. That correction has a small error, so the last value the body saw
// can be slightly ahead of our clock. P must not observe time running
// backwards when it reads the clock natively again: wait the difference out.
void ExportManager::arrive_home(Export& x) {
    XClocks now = read_xclocks();
    uint64_t ahead = 0;
    for (size_t i = 0; i < kXClocks; ++i) {
        // Clocks that never run backwards on one machine. (The wall clocks can
        // be set, so a difference there says nothing.)
        if (kXClockIds[i] != CLOCK_MONOTONIC && kXClockIds[i] != CLOCK_MONOTONIC_RAW && kXClockIds[i] != CLOCK_BOOTTIME) continue;
        if (x.home.clock_last[i] > now[i]) ahead = std::max(ahead, x.home.clock_last[i] - now[i]);
    }
    if (ahead == 0) return finalize_home(x);
    // The error is bounded by the round-trip time of a clock comparison,
    // i.e. far below this; a larger claim is a defect on the other side and
    // is not allowed to keep P frozen.
    constexpr uint64_t kMaxClockWait = 1000000000ull;
    metrics_.observe("export_clock_wait_ns", ahead);
    if (ahead > kMaxClockWait) {
        LOG_WARN("pid %d: its body reports a clock %.3f s ahead of this machine's; not waiting that long", x.pid, double(ahead) / 1e9);
        ahead = kMaxClockWait;
    }
    uint64_t xid = x.xid;
    reactor_.add_timer(ahead, [this, xid] {
        if (Export* x = find(xid)) finalize_home(*x);
    });
}

void ExportManager::finalize_home(Export& x) {
    pid_t pid = x.pid;
    if (!stop_parked(x)) return lost(x, "process vanished while coming home");
    user_regs_struct r;
    if (!regs_from_bytes(x.home.regs, &r)) return lost(x, "malformed register set from the slave");
    r.orig_rax = uint64_t(-1);

    if (x.mid_syscall) {
        // P was stopped by a signal inside the call it was executing for the body.
        user_regs_struct pr;
        (void)pt_getregs(pid, &pr);
        long ax = long(pr.rax);
        if (ax == kErestartSys || ax == kErestartNoIntr || ax == kErestartNoHand || ax == kErestartBlock || ax == -EINTR) {
            // Interrupted before doing anything. Leave the kernel's own
            // bookkeeping in place, with the body's registers and original
            // arguments: the kernel then restarts the call or returns EINTR
            // exactly as it would have for a call P made itself.
            r.rax = pr.rax;
            r.orig_rax = pr.orig_rax;
        } else {
            // The call finished. Its results belong in P's memory now.
            XSysRetMsg unused;
            long rv = collect_results(x, &unused, true);
            for (const XBlob& b : unused.out) { // scatter a vectored read
                KIovec iov[1024];
                uint64_t cnt = std::min<uint64_t>(x.args[x.desc->a[b.arg].len], 1024);
                if (!mem_read(x.mem.get(), x.args[b.arg], iov, cnt * sizeof(KIovec)).is_ok()) continue;
                size_t off = 0;
                for (uint64_t k = 0; k < cnt && off < b.data.size(); ++k) {
                    size_t n = size_t(std::min<uint64_t>(iov[k].len, b.data.size() - off));
                    (void)mem_write(x.mem.get(), iov[k].base, b.data.data() + off, n);
                    off += n;
                }
            }
            r.rax = uint64_t(rv);
            r.rcx = r.rip;
            r.r11 = r.eflags;
        }
    } else if (x.home.at_syscall) {
        // The body stopped at a call that has not been executed: P issues it
        // again, natively this time.
        user_regs_struct br;
        regs_from_bytes(x.home.regs, &br);
        r.rax = br.orig_rax;
        r.rip -= 2;
    }

    // Remove our working area when P is in a plain stop. (Inside a signal
    // stop it cannot be done without disturbing the signal; the mapping is
    // then left behind, unused.)
    if (!x.pending_sig && !x.group_stop && !x.mid_syscall) {
        uint64_t a[6] = {x.scratch, kScratchBytes, 0, 0, 0, 0};
        long ret;
        (void)pt_inject(pid, x.sys_insn, SYS_munmap, a, &ret);
    }
    if (!x.home.xstate.empty()) (void)pt_set_xstate(pid, x.home.xstate);
    (void)pt_setregs(pid, r);
    restore_priority_of(pid, x.home_sched);
    ptrace(PTRACE_DETACH, pid, 0, x.pending_sig);

    metrics_.add("exports_brought_home");
    metrics_.observe("export_duration_ns", now_ns() - x.t0);
    LOG_INFO("pid %d (%s) is home from %s after %.2f s (%s): %" PRIu64 " system calls executed here, %" PRIu64
             " pages sent, %" PRIu64 " modified pages returned",
             pid, x.comm.c_str(), hooks_.machine_name(x.machine).c_str(), double(now_ns() - x.t0) / 1e9, x.why.c_str(),
             x.syscalls, x.pages_out, x.pages_back);
    if (x.pin) {
        uint64_t started = proc_start_time(pid);
        std::lock_guard<std::mutex> lk(meta_mu_);
        pinned_[pid] = started;
    }
    erase(x.xid);
}

void ExportManager::lost(Export& x, const std::string& why) {
    // The body's state cannot be recovered, so P cannot continue correctly.
    LOG_WARN("exported pid %d (%s) is lost (%s): killing it", x.pid, x.comm.c_str(), why.c_str());
    kill(x.pid, SIGKILL);
    // We are its tracer: until we collect it, it would linger as a zombie.
    int st;
    while (waitpid(x.pid, &st, __WALL) < 0 && errno == EINTR) {}
    XSimpleMsg k;
    k.xid = x.xid;
    hooks_.send(x.machine, make_msg(MsgType::XKill, k));
    metrics_.add("exports_lost");
    erase(x.xid);
}

void ExportManager::machine_lost(MachineId machine, const std::string& why, std::vector<std::string>* killed) {
    std::vector<uint64_t> ids;
    for (auto& [id, x] : exports_)
        if (x.machine == machine) ids.push_back(id);
    for (uint64_t id : ids) {
        Export* x = find(id);
        if (!x) continue;
        if (killed) killed->push_back(format("PID %d  %s  (exported native process)", x->pid, x->comm.c_str()));
        lost(*x, why);
    }
}

// ============================ messages from the slave ============================

void ExportManager::on_node_msg(MachineId from, const Msg& m) {
    auto get = [&](uint64_t xid) -> Export* {
        Export* x = find(xid);
        return x && x->machine == from ? x : nullptr;
    };
    switch (m.type) {
    case MsgType::XReady: {
        XReadyMsg r;
        if (!parse_msg(m, &r)) return;
        Export* x = get(r.xid);
        if (!x) return;
        if (r.status != 0) return abort_start(*x, r.text);
        LOG_INFO("pid %d (%s) now executes on %s as CPU %d (body pid %d)", x->pid, x->comm.c_str(),
                 hooks_.machine_name(from).c_str(), x->cpu, r.body_pid);
        bool pin_confirmed = m.req_id == uint64_t(x->cpu) + 1;
        if (x->explicit_cpu && !pin_confirmed) start_home(*x, "agent did not confirm the requested CPU");
        if (hooks_.on_ready) hooks_.on_ready(x->xid, x->pid, x->cpu, x->explicit_cpu && pin_confirmed);
        bool recall_now = x->st == St::Homing;
        park(*x);
        if (recall_now) start_home(*x, x->why);
        return;
    }
    case MsgType::XSyscall: {
        XSyscallMsg s;
        if (!parse_msg(m, &s)) return;
        if (Export* x = get(s.xid)) on_syscall(*x, s);
        return;
    }
    case MsgType::XPageReq: {
        XPageMsg p;
        if (!parse_msg(m, &p)) return;
        if (Export* x = get(p.xid)) on_page_req(*x, p);
        return;
    }
    case MsgType::XHome: {
        XHomeMsg h;
        if (!parse_msg(m, &h)) return;
        Export* x = get(h.xid);
        if (!x) return;
        x->have_home = true;
        if (x->st != St::Homing) {
            x->st = St::Homing;
            x->why = h.reason == XReason::Syscall ? "system call " + h.text : h.reason == XReason::Exit ? "exit" : h.text;
            // A call we do not forward will not become forwardable later.
            if (h.reason == XReason::Syscall) x->pin = true;
        }
        x->exiting = h.reason == XReason::Exit;
        x->home = std::move(h);
        return;
    }
    case MsgType::XPagePush: {
        XPagePushMsg p;
        if (!parse_msg(m, &p)) return;
        Export* x = get(p.xid);
        if (!x || p.data.size() != p.addrs.size() * kPg) return;
        for (size_t i = 0; i < p.addrs.size(); ++i) {
            // Forced write: lands even in mappings that have since become
            // read-only (as a private copy), like a debugger's poke.
            if (mem_write(x->mem.get(), p.addrs[i], p.data.data() + i * kPg, kPg).is_ok()) ++x->pages_back;
            else LOG_ERROR("pid %d: cannot store returned page 0x%" PRIx64, x->pid, p.addrs[i]);
        }
        return;
    }
    case MsgType::XHomeDone: {
        XSimpleMsg d;
        if (!parse_msg(m, &d)) return;
        Export* x = get(d.xid);
        if (x && x->have_home) {
            if (x->pages_back != d.count) return lost(*x, "could not restore every modified page on recall");
            arrive_home(*x);
        }
        return;
    }
    case MsgType::XGone: {
        XSimpleMsg g;
        if (!parse_msg(m, &g)) return;
        if (Export* x = get(g.xid)) lost(*x, g.text);
        return;
    }
    default:
        return;
    }
}

} // namespace dnuma
