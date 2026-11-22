// SPDX-License-Identifier: GPL-2.0-only
// Policy for process export: which slaves can host native processes, how
// their CPUs are numbered on the master, and the automatic balancer.
#include <algorithm>
#include <cinttypes>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"
#include "master/master.h"

namespace dnuma {

namespace {
constexpr uint64_t kMs = 1000000ull;
constexpr unsigned long kPfKthread = 0x00200000;

struct ProcStat {
    char state = 0;
    unsigned long flags = 0;
    uint64_t cputime = 0; // utime + stime, clock ticks
    long threads = 0;
    uint64_t start = 0;
};

bool read_proc_stat(pid_t pid, ProcStat* out) {
    std::string s = read_attr("/proc/" + std::to_string(pid) + "/stat");
    size_t paren = s.rfind(')'); // the command name may contain anything
    if (paren == std::string::npos || paren + 2 >= s.size()) return false;
    std::vector<std::string> f = split(s.substr(paren + 2), ' ');
    if (f.size() < 20) return false;
    out->state = f[0][0];
    out->flags = strtoul(f[6].c_str(), nullptr, 10);
    out->cputime = strtoull(f[11].c_str(), nullptr, 10) + strtoull(f[12].c_str(), nullptr, 10);
    out->threads = strtol(f[17].c_str(), nullptr, 10);
    out->start = strtoull(f[19].c_str(), nullptr, 10);
    return true;
}
} // namespace

void Master::export_init() {
    ExportManager::Hooks h;
    h.send = [this](MachineId m, Msg msg) { send_to_machine(m, std::move(msg)); };
    h.machine_name = [this](MachineId m) { return machine_name(m); };
    h.on_ready = [this](uint64_t xid, pid_t pid, int cpu, bool pinned) { view_task_started(xid, pid, cpu, pinned); };
    h.on_end = [this](uint64_t xid, MachineId m, pid_t pid) {
        // Deferred: the manager is in the middle of forgetting the export.
        reactor_.post([this, xid, m, pid] {
            view_task_ended(xid, pid);
            Slave* s = slave_by_machine(m);
            if (s && s->state == SlaveState::Draining) drain_step(m);
            if (shutdown_done_ && exports_ && exports_->count() == 0) {
                auto done = std::move(shutdown_done_);
                shutdown_done_ = nullptr;
                shutdown();
                done();
            }
        });
    };
    exports_ = std::make_unique<ExportShards>(reactor_, std::move(h), cfg_.export_workers);
    // Slave CPUs are numbered after every CPU this machine could ever have,
    // so a number never means two things (and the kernel's view requires it).
    std::vector<int> possible = parse_cpulist(read_attr("/sys/devices/system/cpu/possible"));
    next_cpu_base_ = res_.cpus_online;
    for (int c : possible) next_cpu_base_ = std::max(next_cpu_base_, c + 1);
    view_init();
    if (cfg_.export_auto) {
        reactor_.add_timer(uint64_t(cfg_.export_tick_ms) * kMs, [this] { export_tick(); }, uint64_t(cfg_.export_tick_ms) * kMs);
        LOG_INFO("automatic process export is on (processes of uid >= %u)", cfg_.export_uid_min);
    }
}

void Master::on_sigchld() {
    if (exports_) exports_->on_sigchld();
}

void Master::request_shutdown(std::function<void()> done) {
    size_t n = exports_ ? exports_->count() : 0;
    if (n == 0 || shutdown_done_) {
        shutdown();
        return done();
    }
    // Processes whose execution is on a slave come home before we go away.
    LOG_INFO("bringing %zu exported process(es) home before shutting down", n);
    shutdown_done_ = std::move(done);
    for (auto& [name, s] : slaves_)
        if (s->machine != kNoMachine) exports_->recall_all(s->machine, "master shutting down");
    reactor_.add_timer(5000 * kMs, [this] {
        if (!shutdown_done_) return;
        LOG_WARN("some exported processes did not come home in time");
        auto done = std::move(shutdown_done_);
        shutdown_done_ = nullptr;
        shutdown();
        done();
    });
}

// Decides, at attach, whether native processes may execute on this slave and
// which CPU numbers its CPUs get on the master.
void Master::export_slave_attached(Slave& s) {
    s.export_block.clear();
    // A process that started here has already looked at this machine's CPU
    // features; every one of them must exist on the slave too.
    std::string missing;
    for (const std::string& f : res_.cpu_flags)
        if (!std::binary_search(s.res.cpu_flags.begin(), s.res.cpu_flags.end(), f)) {
            // Flags that describe the kernel's mitigations or the hypervisor,
            // not instructions a program can use.
            static const char* soft[] = {"pti", "ssbd", "ibrs", "ibpb", "stibp", "md_clear", "flush_l1d", "ibpb_exit_to_user",
                                         "tpr_shadow", "vnmi", "flexpriority", "ept", "vpid", "ept_ad", "arch_capabilities",
                                         "hypervisor", "cpuid_fault", "invpcid_single", "retpoline", "rsb_ctxsw", "ibrs_enhanced"};
            bool is_soft = false;
            for (const char* k : soft) is_soft = is_soft || f == k;
            if (!is_soft) missing += (missing.empty() ? "" : " ") + f;
        }
    if (!missing.empty()) s.export_block = "its CPUs lack features this machine's processes may be using: " + missing;
    else if (s.res.xcr0 != res_.xcr0 || s.res.xsave_size != res_.xsave_size)
        s.export_block = format("different extended register state layout (xcr0 0x%" PRIx64 " vs 0x%" PRIx64 ")", s.res.xcr0, res_.xcr0);

    // The numbers stay the same across reattachments, unless the slave comes
    // back offering more CPUs than the range reserved for it holds.
    auto it = cpu_base_by_uuid_.find(s.uuid);
    if (it == cpu_base_by_uuid_.end() || it->second.second < s.res.cpus_online) {
        s.cpu_base = next_cpu_base_;
        next_cpu_base_ += s.res.cpus_online;
        cpu_base_by_uuid_[s.uuid] = {s.cpu_base, s.res.cpus_online};
    } else {
        s.cpu_base = it->second.first;
    }
    view_slave_attached(s);
    if (s.export_block.empty())
        LOG_INFO("slave %s can execute native processes; its CPUs are numbered %d-%d here", s.name.c_str(), s.cpu_base,
                 s.cpu_base + s.res.cpus_online - 1);
    else
        LOG_WARN("slave %s cannot execute native processes: %s", s.name.c_str(), s.export_block.c_str());
}

Slave* Master::export_target(const std::string& name, std::string* why, bool require_idle) {
    Slave* best = nullptr;
    size_t best_free = 0;
    for (auto& [n, s] : slaves_) {
        if (!name.empty() && n != name) continue;
        if (s->state != SlaveState::Active) {
            *why = n + " is " + to_string(s->state);
            continue;
        }
        if (!s->export_block.empty()) {
            *why = n + ": " + s->export_block;
            continue;
        }
        size_t used = exports_->count(s->machine);
        size_t free = size_t(s->res.cpus_online) > used ? size_t(s->res.cpus_online) - used : 0;
        if (require_idle && free == 0) {
            *why = n + " has no idle CPU";
            continue;
        }
        if (!best || free > best_free) {
            best = s.get();
            best_free = free;
        }
    }
    if (!best && why->empty()) *why = name.empty() ? "no slave is attached" : "unknown slave '" + name + "'";
    return best;
}

// The slave CPU with the fewest exported processes on it.
int Master::export_pick_cpu(Slave& s) {
    std::vector<int> load(size_t(s.res.cpus_online), 0);
    for (const auto& info : exports_->list())
        if (info.machine == s.machine && info.cpu >= s.cpu_base && info.cpu - s.cpu_base < int(load.size()))
            ++load[size_t(info.cpu - s.cpu_base)];
    return int(std::min_element(load.begin(), load.end()) - load.begin());
}

Status Master::export_pid(pid_t pid, Slave& s, ExportPlacement placement, int cluster_cpu) {
    if (pid <= 1 || pid == getpid()) return Status::err(EINVAL, "not a process that can be exported");
    for (auto& [id, p] : procs_)
        if (p.home_pid == pid) return Status::err(EINVAL, "is a dnuma-run program (it places its own threads)");
    if (cluster_cpu >= 0 && (cluster_cpu < s.cpu_base || cluster_cpu - s.cpu_base >= s.res.cpus_online))
        return Status::err(EINVAL, "CPU %d does not belong to %s", cluster_cpu, s.name.c_str());
    int idx = cluster_cpu >= 0 ? cluster_cpu - s.cpu_base : export_pick_cpu(s);
    // Map the CPU index to the slave's actual CPU number and NUMA node.
    int local_cpu = -1, logical_node = -1;
    std::vector<int> cpus;
    for (const auto& n : s.res.nodes)
        for (int c : n.cpus) cpus.push_back(c);
    std::sort(cpus.begin(), cpus.end());
    if (idx < 0 || size_t(idx) >= cpus.size()) return Status::err(ENODEV, "CPU missing from agent topology");
    local_cpu = cpus[size_t(idx)];
    for (const auto& n : s.res.nodes) {
        if (std::find(n.cpus.begin(), n.cpus.end(), local_cpu) != n.cpus.end())
            for (const auto& a : s.nodes)
                if (a.phys_index == n.phys_index) logical_node = a.logical;
    }
    return exports_->start(pid, s.machine, s.cpu_base + idx, logical_node, local_cpu, placement);
}

// The balancer: when this machine has more runnable work than CPUs and a
// slave has idle CPUs, move CPU-bound single-threaded processes there. It
// only *starts* exports; a process comes home by itself when it needs to
// (signal, fork, exec, a call that cannot be forwarded) or when its slave is
// drained.
void Master::export_tick() {
    if (shutting_down_ || shutdown_done_) return;
    // Remember CPU times every tick so that "CPU-bound" is a measured fact.
    uint32_t runnable = runnable_now();
    int over = int(runnable) - 1 - res_.cpus_online; // minus ourselves
    std::string why;
    Slave* any = over > 0 ? export_target("", &why) : nullptr;
    if (!any) {
        // Not overloaded, or nowhere to send anything: do not walk /proc.
        export_cputime_.clear();
        return;
    }

    std::map<pid_t, uint64_t> seen;
    std::vector<std::pair<uint64_t, pid_t>> candidates; // (cpu ticks used this interval, pid)
    DIR* d = opendir("/proc");
    if (!d) return;
    long hz = sysconf(_SC_CLK_TCK);
    uint64_t busy_ticks = uint64_t(cfg_.export_tick_ms) * uint64_t(hz) / 1000 / 2; // at least half the interval on a CPU
    if (busy_ticks == 0) busy_ticks = 1;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t pid = atoi(e->d_name);
        ProcStat ps;
        if (!read_proc_stat(pid, &ps)) continue;
        if (ps.flags & kPfKthread) continue;
        seen[pid] = ps.cputime;
        auto prev = export_cputime_.find(pid);
        if (!any || prev == export_cputime_.end()) continue; // first sighting: nothing to compare with
        uint64_t used = ps.cputime - std::min(ps.cputime, prev->second);
        if (ps.state != 'R' || ps.threads != 1 || used < busy_ticks) continue;
        if (pid == getpid() || exports_->is_exported(pid) || exports_->is_pinned(pid)) continue;
        auto skip = export_skip_.find(pid);
        if (skip != export_skip_.end() && skip->second == ps.start) continue;
        struct stat st{};
        if (stat(("/proc/" + std::string(e->d_name)).c_str(), &st) != 0 || st.st_uid < cfg_.export_uid_min) continue;
        candidates.emplace_back(used, pid);
    }
    closedir(d);
    export_cputime_.swap(seen);
    for (auto it = export_skip_.begin(); it != export_skip_.end();)
        it = export_cputime_.count(it->first) ? std::next(it) : export_skip_.erase(it);
    if (!any || candidates.empty()) return;

    std::sort(candidates.rbegin(), candidates.rend()); // busiest first
    int moved = 0;
    for (const auto& [used, pid] : candidates) {
        if (moved >= over) break;
        Slave* s = export_target("", &why);
        if (!s) break;
        Status st = export_pid(pid, *s, ExportPlacement::Balance);
        if (st.is_ok()) {
            ++moved;
            metrics_.add("exports_by_balancer");
        } else if (st.code() != EAGAIN) {
            // Not exportable as it is (threads, shared memory, ...): do not
            // examine it again on every tick.
            LOG_DEBUG("pid %d not exported: %s", pid, st.str().c_str());
            export_skip_[pid] = proc_start_time(pid);
        }
    }
}

std::string Master::render_exports() {
    std::string out = "\n" + render_view();
    out += "\nExported native processes (executing on a slave, system calls served here):\n";
    auto list = exports_->list();
    if (list.empty()) return out + "    (none)\n";
    out += format("    %-8s %-18s %-6s %-12s %-5s %-12s %-10s %-10s %s\n", "PID", "COMMAND", "UID", "ON", "CPU", "STATE", "SYSCALLS",
                  "PAGES-OUT", "FOR");
    for (const auto& i : list)
        out += format("    %-8d %-18s %-6u %-12s %-5d %-12s %-10" PRIu64 " %-10" PRIu64 " %.1fs\n", i.pid, i.comm.c_str(), i.uid,
                      machine_name(i.machine).c_str(), i.cpu, i.state.c_str(), i.syscalls, i.pages_out, double(i.age_ms) / 1000.0);
    return out;
}

} // namespace dnuma
