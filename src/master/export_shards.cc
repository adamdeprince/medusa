// SPDX-License-Identifier: GPL-2.0-only
#include "master/export_shards.h"

#include <future>

#include "common/log.h"

namespace dnuma {

template <class F> void ExportShards::on_shard(Shard& s, F&& f) const {
    std::promise<void> done;
    s.reactor.post([&] {
        f();
        done.set_value();
    });
    done.get_future().wait();
}

ExportShards::ExportShards(Reactor& main, Hooks hooks, unsigned threads) : main_(main), hooks_(std::move(hooks)) {
    if (threads == 0) threads = WorkerPool::default_threads(std::thread::hardware_concurrency());
    pool_ = std::make_unique<WorkerPool>(threads, [] { raise_priority(); });

    // What a worker's manager may call, from its own thread.
    Hooks h;
    std::weak_ptr<bool> alive = alive_;
    h.send = [this, alive](MachineId m, Msg msg) {
        auto box = std::make_shared<Msg>(std::move(msg)); // moved, never copied, on its way to the link
        main_.post([this, alive, m, box] {
            auto a = alive.lock();
            if (a && *a) hooks_.send(m, std::move(*box));
        });
    };
    h.machine_name = [this](MachineId m) {
        std::lock_guard<std::mutex> lk(names_mu_);
        auto it = names_.find(m);
        return it == names_.end() ? "machine " + std::to_string(m) : it->second;
    };
    h.on_ready = [this, alive](uint64_t xid, pid_t pid, int cpu, bool pinned) {
        main_.post([this, alive, xid, pid, cpu, pinned] {
            auto a = alive.lock();
            if (a && *a && hooks_.on_ready) hooks_.on_ready(xid, pid, cpu, pinned);
        });
    };
    h.on_end = [this, alive](uint64_t xid, MachineId m, pid_t pid) {
        main_.post([this, alive, xid, m, pid] {
            auto a = alive.lock();
            if (a && *a && hooks_.on_end) hooks_.on_end(xid, m, pid);
        });
    };

    for (unsigned i = 0; i < threads; ++i) {
        auto sh = std::make_unique<Shard>();
        // Export ids say which worker owns them: id % threads.
        sh->mgr = std::make_unique<ExportManager>(sh->reactor, sh->metrics, h, *pool_, i == 0 ? threads : i, threads);
        Shard* s = sh.get();
        s->reactor.set_slow_hook(5000000, [i](const char* kind, int fd, uint64_t ns) {
            LOG_WARN("export worker %u stalled %.1f ms in a %s (fd %d)", i, double(ns) / 1e6, kind, fd);
        });
        s->thread = std::thread([s] {
            raise_priority(); // not inherited from the main thread
            s->reactor.run();
        });
        shards_.push_back(std::move(sh));
    }
}

ExportShards::~ExportShards() {
    *alive_ = false;
    for (auto& sh : shards_) {
        Shard* s = sh.get();
        s->reactor.post([s] {
            s->mgr.reset(); // on its own thread: it is the tracer of its processes
            s->reactor.stop();
        });
        if (s->thread.joinable()) s->thread.join();
    }
}

Status ExportShards::start(pid_t pid, MachineId machine, int cpu, int node, int local_cpu, ExportPlacement placement) {
    if (is_exported(pid)) return Status::err(EALREADY, "already exported");
    {
        std::string name = hooks_.machine_name(machine);
        std::lock_guard<std::mutex> lk(names_mu_);
        names_[machine] = std::move(name);
    }
    Shard& s = *shards_[next_++ % shards_.size()];
    Status st = Status::ok();
    on_shard(s, [&] { st = s.mgr->start(pid, machine, cpu, node, local_cpu, placement); });
    return st;
}

Status ExportShards::recall(pid_t pid, const std::string& why, bool pin) {
    for (auto& sh : shards_) {
        if (!sh->mgr->is_exported(pid)) continue;
        Status st = Status::ok();
        on_shard(*sh, [&] { st = sh->mgr->recall(pid, why, pin); });
        return st;
    }
    return Status::err(ESRCH, "pid %d is not exported", pid);
}

void ExportShards::recall_all(MachineId machine, const std::string& why) {
    for (auto& sh : shards_) {
        Shard* s = sh.get();
        s->reactor.post([s, machine, why] { s->mgr->recall_all(machine, why); });
    }
}

void ExportShards::machine_lost(MachineId machine, const std::string& why, std::vector<std::string>* killed) {
    for (auto& sh : shards_) on_shard(*sh, [&] { sh->mgr->machine_lost(machine, why, killed); });
}

void ExportShards::on_node_msg(MachineId from, Msg&& m) {
    if (m.type == MsgType::XClockReq) {
        // Answered right here: every hop would add to the measurement error.
        XClockMsg c;
        if (!parse_msg(m, &c)) return;
        c.master = read_xclocks();
        hooks_.send(from, make_msg(MsgType::XClock, c));
        return;
    }
    // Every other export message starts with the export id.
    if (m.payload.size() < 8) return;
    uint64_t xid;
    memcpy(&xid, m.payload.data(), 8);
    Shard* s = shards_[xid % shards_.size()].get();
    auto box = std::make_shared<Msg>(std::move(m));
    s->reactor.post([s, from, box] { s->mgr->on_node_msg(from, *box); });
}

void ExportShards::on_sigchld() {
    for (auto& sh : shards_) {
        Shard* s = sh.get();
        s->reactor.post([s] { s->mgr->on_sigchld(); });
    }
}

size_t ExportShards::count(MachineId machine) const {
    size_t n = 0;
    for (const auto& sh : shards_) n += sh->mgr->count(machine);
    return n;
}

size_t ExportShards::count() const {
    size_t n = 0;
    for (const auto& sh : shards_) n += sh->mgr->count();
    return n;
}

bool ExportShards::is_exported(pid_t pid) const {
    for (const auto& sh : shards_)
        if (sh->mgr->is_exported(pid)) return true;
    return false;
}

bool ExportShards::is_pinned(pid_t pid) const {
    for (const auto& sh : shards_)
        if (sh->mgr->is_pinned(pid)) return true;
    return false;
}

std::vector<ExportShards::Info> ExportShards::list() const {
    std::vector<Info> out;
    for (const auto& sh : shards_) {
        on_shard(*sh, [&] {
            std::vector<Info> part = sh->mgr->list();
            out.insert(out.end(), part.begin(), part.end());
        });
    }
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.pid < b.pid; });
    return out;
}

std::vector<std::string> ExportShards::describe(MachineId machine) const {
    std::vector<std::string> out;
    for (const auto& sh : shards_) {
        on_shard(*sh, [&] {
            std::vector<std::string> part = sh->mgr->describe(machine);
            out.insert(out.end(), part.begin(), part.end());
        });
    }
    return out;
}

void ExportShards::merge_metrics(Metrics* into) const {
    for (const auto& sh : shards_) on_shard(*sh, [&] { into->merge(sh->metrics); });
}

} // namespace dnuma
