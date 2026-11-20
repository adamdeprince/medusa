/* SPDX-License-Identifier: GPL-2.0-only */
// BodyRunner: the slave side of process export.
//
// A *body* is a process on the slave that executes the user-mode code of a
// native process whose home is the master. It has the same address-space
// layout and registers as the original, but:
//   * its memory starts empty and is filled on demand with pages read from
//     the original process on the master;
//   * it runs under PTRACE_SYSEMU, so every system call it makes stops it
//     before the local kernel acts; the call is sent to the master and
//     executed there by the original process;
//   * pages it modifies are tracked (userfaultfd write protection) so that
//     exactly those are sent back when it goes home.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/user.h>

#include "common/metrics.h"
#include "common/reactor.h"
#include "export/ptrace_util.h"
#include "export/sysdesc.h"
#include "export/xmsg.h"

namespace dnuma {

// The measured difference between the master's clocks and this machine's.
//
// Programs read the time constantly; asking the master each time would make
// every such read a network round trip. Instead the agent compares clocks
// with the master once a second (XClockReq/XClock: read ours, the master
// reads its own, read ours again) and bodies are given "our clock + the
// difference". A comparison's error is at most half its round-trip time, and
// grows with age because the two machines' clocks drift apart; the estimate
// with the smallest current error bound is used.
//
// Written by the agent's main thread, read by the worker threads.
class ClockSync {
public:
    using Offsets = std::array<int64_t, kXClocks>;
    void add(const XClocks& sent, const XClocks& master, const XClocks& received);
    bool get(Offsets* off, uint64_t* err_ns) const; // false: no comparison yet
    bool empty() const;

private:
    struct Sample {
        uint64_t at = 0, rtt = 0; // on this machine's CLOCK_MONOTONIC
        Offsets off{};
    };
    static uint64_t error_at(const Sample& s, uint64_t now);
    mutable std::mutex mu_;
    std::vector<Sample> samples_; // only those that can still become the best one
};

class BodyRunner {
public:
    BodyRunner(Reactor& reactor, Metrics& metrics, std::string stub_path, std::function<void(Msg)> to_master,
               std::shared_ptr<const ClockSync> clocks);
    ~BodyRunner();

    void handle(Msg&& m);  // XBegin, XSysRet, XPage, XStop, XKill
    void on_sigchld();     // some child changed state: check every body
    void kill_all();       // fencing: destroy every body
    uint32_t count() const { return count_.load(std::memory_order_relaxed); } // any thread
    // Appends (export id, pid) of every body that has a process. Any thread.
    void pids(std::vector<std::pair<uint64_t, pid_t>>* out) const;

private:
    enum class St { Running, AwaitPages, AwaitReply, Stopped };
    enum class Step { Done, Wait, Fault };

    struct Body {
        uint64_t xid = 0;
        pid_t pid = 0;
        Fd uffd, mem;
        uint64_t gadget = 0;
        int cpu = -1, node = -1;
        std::map<uint64_t, XVma> vmas;                  // by start address
        std::unordered_map<uint64_t, uint8_t> pages;    // page address -> 1 clean, 2 modified
        std::unordered_map<uint64_t, uint32_t> requested; // request start -> page count
        std::unordered_map<uint64_t, uint64_t> req_t0;    // request start -> time sent
        std::unordered_map<uint64_t, bool> req_write;     // request start -> triggered by a write
        uint64_t sys_t0 = 0, stop_t0 = 0;
        St st = St::Running;
        user_regs_struct sregs{};                       // registers at the pending system call
        const SysDesc* desc = nullptr;
        uint64_t seq = 0;
        XSysRetMsg reply;                               // reply being applied
        bool have_reply = false;
        uint64_t faulted_page = 0;                      // page the master refused
        uint64_t rseq = 0;                              // address of the process's rseq area, if any
        bool rseq_done = false;
        uint64_t t0 = 0, syscalls = 0, faults = 0, pages_in = 0;
        uint64_t ra_next = 0;                           // page after the last one requested
        uint32_t ra_window = 0;                         // pages asked for by that request
        XClocks clock_floor{}, clock_last{};            // see local_time()
    };

    void begin(const XBeginMsg& b);
    Status spawn(Body& body, const XBeginMsg& b);
    void destroy(uint64_t xid);
    Body* find(uint64_t xid);

    void handle_stop(Body& b, int status);
    void resume(Body& b);
    void on_syscall(Body& b);
    void continue_syscall(Body& b);
    Step gather(Body& b, XSyscallMsg* out);
    void finish_syscall(Body& b, long ret);
    void apply_reply(Body& b);
    Step write_out(Body& b, uint64_t addr, const uint8_t* data, size_t len);
    Step local_syscall(Body& b, long* ret);
    bool local_time(Body& b, long* ret, Step* step);
    void go_home(Body& b, XReason reason, int64_t nr, const std::string& text);

    void on_uffd(uint64_t xid);
    void request_page(Body& b, uint64_t page, bool for_write = false);
    void on_page(Body& b, const XPageView& p);
    Step need_range(Body& b, uint64_t addr, uint64_t len); // all pages present?
    const XVma* find_vma(Body& b, uint64_t addr);
    bool range_writable(Body& b, uint64_t addr, uint64_t len);
    void vma_remove(Body& b, uint64_t start, uint64_t end);
    Status map_vma(Body& b, const XVma& v);
    void mark_dirty(Body& b, uint64_t page);
    Status inject(Body& b, long nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, long* ret);
    uint64_t cpu_time_ns(Body& b);

    Reactor& reactor_;
    Metrics& metrics_;
    std::string stub_path_;
    std::function<void(Msg)> to_master_;
    std::shared_ptr<const ClockSync> clocks_;
    std::map<uint64_t, std::unique_ptr<Body>> bodies_;
    std::map<pid_t, uint64_t> by_pid_;
    std::atomic<uint32_t> count_{0};
    mutable std::mutex pids_mu_;
    std::map<uint64_t, pid_t> pids_; // for pids(); guarded by pids_mu_
    // All bodies share one process group (led by an idle helper process), so
    // their state changes can be collected with a single wait call without
    // touching the agent's other children.
    pid_t group_ = 0;
    void ensure_group();
};

// Runs bodies on several worker threads. Hosting a body is per-process
// service work (its faults, its system calls, its ptrace stops); one thread
// cannot keep up with many busy bodies. Each worker has its own event loop
// and its own BodyRunner; a body belongs to one worker for its whole life,
// which is also what ptrace requires (only the thread that started a tracee
// may control it). All methods are called from the agent's main thread.
class BodyShards {
public:
    BodyShards(Reactor& main, std::string stub_path, std::function<void(Msg)> to_master, unsigned workers);
    ~BodyShards();

    void handle(Msg&& m);
    void on_sigchld();
    void kill_all(); // returns when every body is gone
    uint32_t count() const;
    std::vector<std::pair<uint64_t, pid_t>> pids() const; // (export id, pid) of every body
    void merge_metrics(Metrics* into); // adds the workers' most recently published metrics

private:
    struct Shard {
        Reactor reactor;
        Metrics metrics;   // worker thread only
        std::mutex pub_mu;
        Metrics published; // copy for other threads
        std::unique_ptr<BodyRunner> runner;
        std::thread thread;
    };
    std::vector<std::unique_ptr<Shard>> shards_;
    Reactor& main_;
    std::function<void(Msg)> to_master_;
    std::shared_ptr<ClockSync> clocks_ = std::make_shared<ClockSync>();
    Reactor::TimerId clock_timer_ = 0;
    void compare_clocks();
};

} // namespace dnuma
