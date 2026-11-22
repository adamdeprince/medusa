/* SPDX-License-Identifier: GPL-2.0-only */
// ExportShards: process export spread over several threads.
//
// The home side of an exported process is per-process service work: every
// system call its body makes is marshalled, executed and collected here. One
// thread cannot keep up with many busy processes, so they are divided among
// worker threads, each with its own event loop and its own ExportManager. A
// process belongs to one worker for as long as it is exported - which is also
// what ptrace requires (only the thread that attached may control a tracee).
//
// All methods are called from the master's main thread. Messages to slaves
// and notifications leave through the main thread as well.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/workers.h"
#include "master/export.h"

namespace dnuma {

class ExportShards {
public:
    using Hooks = ExportManager::Hooks;
    using Info = ExportManager::Info;

    // `threads` worker threads execute system calls, and as many again read
    // process memory for page requests (0 = WorkerPool::default_threads).
    ExportShards(Reactor& main, Hooks hooks, unsigned threads);
    ~ExportShards();

    Status start(pid_t pid, MachineId machine, int cpu, int node, int local_cpu, ExportPlacement placement);
    Status recall(pid_t pid, const std::string& why, bool pin);
    void recall_all(MachineId machine, const std::string& why);
    void machine_lost(MachineId machine, const std::string& why, std::vector<std::string>* killed);

    void on_node_msg(MachineId from, Msg&& m);
    void on_sigchld();

    size_t count(MachineId machine) const;
    size_t count() const;
    bool is_exported(pid_t pid) const;
    bool is_pinned(pid_t pid) const;
    std::vector<Info> list() const;
    std::vector<std::string> describe(MachineId machine) const;
    void merge_metrics(Metrics* into) const; // adds the workers' counters and distributions

private:
    struct Shard {
        Reactor reactor;
        Metrics metrics; // worker thread only
        std::unique_ptr<ExportManager> mgr;
        std::thread thread;
    };
    // Runs f on the shard's thread and waits for it. (Workers never wait for
    // the main thread, so this cannot deadlock.)
    template <class F> void on_shard(Shard& s, F&& f) const;

    Reactor& main_;
    Hooks hooks_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    mutable std::mutex names_mu_;
    std::map<MachineId, std::string> names_; // for log messages written by workers
    size_t next_ = 0;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::unique_ptr<WorkerPool> pool_; // after shards_: destroyed first, while their event loops still exist
};

} // namespace dnuma
