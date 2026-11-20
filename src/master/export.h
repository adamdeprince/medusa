/* SPDX-License-Identifier: GPL-2.0-only */
// ExportManager: the master side of process export.
//
// An ordinary process ("P") that started on the master can have its
// user-mode execution moved to a slave while everything the kernel knows
// about it stays here. P itself never goes away: it is held with ptrace and
//   * supplies its memory, page by page, as the body on the slave touches it;
//   * executes every system call the body makes - in its own context, with
//     its own descriptors, credentials, working directory and signal state;
//   * receives signals as usual; anything that needs P's real user state
//     (a signal with a handler, a fault, fork, exec, an unsupported call)
//     brings the execution home first and then proceeds natively.
// Bringing it home copies back the registers and exactly the pages the body
// modified, and detaches; from then on P is an ordinary process again.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <sys/user.h>

#include "common/hwprobe.h"
#include "common/metrics.h"
#include <mutex>

#include "common/reactor.h"
#include "common/workers.h"
#include "common/util.h"
#include "export/ptrace_util.h"
#include "export/sysdesc.h"
#include "export/xmsg.h"

namespace dnuma {

class ExportManager {
public:
    struct Hooks {
        std::function<void(MachineId, Msg)> send;          // operations channel to a slave
        std::function<std::string(MachineId)> machine_name;
        // Called whenever an export ends (for drain progress and accounting).
        std::function<void(MachineId, pid_t)> on_end;
    };
    struct Info {
        uint64_t xid;
        pid_t pid;
        std::string comm, state;
        uint32_t uid;
        MachineId machine;
        int cpu;
        uint64_t syscalls, pages_out, age_ms;
    };

    // One manager serves the processes of one thread (see ExportShards).
    // `pool` reads process memory for page requests. Export ids are
    // xid_first, xid_first + xid_step, ...
    //
    // Everything is called on the manager's own thread, except the queries
    // marked "any thread".
    ExportManager(Reactor& reactor, Metrics& metrics, Hooks hooks, WorkerPool& pool, uint64_t xid_first = 1, uint64_t xid_step = 1);
    ~ExportManager();

    // Starts exporting `pid` to `machine`. `cpu`/`node` are how it is
    // presented; `local_cpu` is the slave CPU it is pinned to.
    Status start(pid_t pid, MachineId machine, int cpu, int node, int local_cpu);
    // Brings one exported process home. `pin` keeps it from being picked again.
    Status recall(pid_t pid, const std::string& why, bool pin);
    void recall_all(MachineId machine, const std::string& why);
    // The slave is gone: bodies and what they modified are lost, so the
    // processes are killed. Appends a line per process to *killed.
    void machine_lost(MachineId machine, const std::string& why, std::vector<std::string>* killed);

    void on_node_msg(MachineId from, const Msg& m);
    void on_sigchld();

    size_t count(MachineId machine) const; // any thread
    size_t count() const;                  // any thread
    bool is_exported(pid_t pid) const;     // any thread
    bool is_pinned(pid_t pid) const;       // any thread
    std::vector<Info> list() const;
    std::vector<std::string> describe(MachineId machine) const;

private:
    // Parked: asleep in ppoll, signals are noticed at once.
    // Stopping: asked to stop so that a call can be run; the request is kept.
    // InSyscall: executing a call for its body.
    // Idle: stopped right after a call, in case the next one follows at once.
    enum class St { Starting, Parked, Stopping, InSyscall, Idle, Homing };
    struct OutPlan {
        uint8_t arg;
        ArgKind kind;
        uint64_t addr; // in the scratch area
        uint64_t cap;
    };
    struct Export {
        uint64_t xid = 0;
        pid_t pid = 0;
        std::string comm;
        uint32_t uid = 0, gid = 0;
        MachineId machine = 0;
        int cpu = -1;
        St st = St::Starting;
        Fd mem;
        std::shared_ptr<Fd> mem_shared; // a second descriptor, for the page workers
        uint64_t sys_insn = 0;      // a `syscall` instruction that exists in P anyway
        uint64_t scratch = 0;       // our mapping in P: gadget page + data area
        uint64_t gadget = 0;
        uint64_t brk = 0;
        uint64_t vdso_start = 0, vdso_end = 0;
        std::vector<uint8_t> vdso;  // patched copy served to the body
        std::shared_ptr<const std::vector<uint8_t>> vdso_shared; // the same, for the page workers
        user_regs_struct orig_regs{};
        // System call in flight.
        uint64_t seq = 0;
        int64_t nr = 0;
        const SysDesc* desc = nullptr;
        uint64_t args[6] = {};
        std::vector<OutPlan> outs;
        // Coming home.
        bool stopped = false;       // P is in a ptrace stop right now
        bool group_stop = false;
        bool mid_syscall = false;   // stopped by a signal inside the injected call
        int pending_sig = 0;
        bool have_home = false, pin = false, exiting = false;
        XHomeMsg home;
        std::string why;
        uint64_t t0 = 0, syscalls = 0, pages_out = 0, pages_back = 0;
        uint64_t sys_t0 = 0;
        XSyscallMsg pending;        // the call waiting for P to stop
        Reactor::TimerId idle_timer = 0;
        SchedState home_sched;      // P's own scheduling settings, restored when it comes home
    };

    Export* find(uint64_t xid);
    Status bootstrap(Export& x, std::vector<XVma>* vmas, std::vector<uint8_t>* xstate);
    void abort_start(Export& x, const std::string& why);
    void park(Export& x);
    bool stop_parked(Export& x);
    void handle_wait(Export& x, int status);
    bool ignorable(pid_t pid, int sig);
    void on_syscall(Export& x, const XSyscallMsg& m);
    void run_syscall(Export& x, const XSyscallMsg& m);
    void arrive_home(Export& x);
    void idle(Export& x);
    void complete_syscall(Export& x);
    long collect_results(Export& x, XSysRetMsg* ret, bool into_process);
    void on_page_req(Export& x, const XPageMsg& rq);
    void start_home(Export& x, const std::string& why);
    void finalize_home(Export& x);
    void lost(Export& x, const std::string& why);
    void erase(uint64_t xid);

    Reactor& reactor_;
    Metrics& metrics_;
    Hooks hooks_;
    std::map<uint64_t, Export> exports_;
    std::map<pid_t, uint64_t> by_pid_;
    std::map<pid_t, uint64_t> pinned_; // pid -> process start time (so a reused pid is not pinned)
    uint64_t next_xid_ = 1, xid_step_ = 1;
    // Held to add or remove entries of the three maps above, and by other
    // threads to look at them (never at the mutable state of an export).
    mutable std::mutex meta_mu_;
    // Results of page jobs come back as posted tasks, possibly after this
    // object is gone; they check this first.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    WorkerPool& pool_;
};

// Process start time in clock ticks since boot (field 22 of /proc/pid/stat).
uint64_t proc_start_time(pid_t pid);

} // namespace dnuma
