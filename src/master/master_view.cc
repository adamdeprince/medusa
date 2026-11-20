// SPDX-License-Identifier: GPL-2.0-only
// The master's /proc view of slave CPUs and of the work exported processes
// do on them: what is written to the kernel (master/kernel_view.h), and when.
#include <algorithm>
#include <cinttypes>

#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"
#include "master/master.h"

namespace dnuma {

namespace {
// The slave's CPU numbers in the order they are numbered on the master
// (CPU `cpu_base + i` here is element i).
std::vector<int> slave_cpus(const Slave& s) {
    std::vector<int> cpus;
    for (const auto& n : s.res.nodes) cpus.insert(cpus.end(), n.cpus.begin(), n.cpus.end());
    std::sort(cpus.begin(), cpus.end());
    return cpus;
}
} // namespace

void Master::view_init() {
    if (!cfg_.kernel_view) {
        LOG_INFO("kernel view: off (--no-kernel-view)");
        return;
    }
    std::string why;
    if (!view_.open(&why)) {
        LOG_INFO("kernel view: not available: %s. Slave CPUs will not appear in /proc/cpuinfo, /proc/stat or top", why.c_str());
        return;
    }
    next_cpu_base_ = std::max(next_cpu_base_, view_.nr_cpu_ids());
    next_phys_shift_ = max_physical_id(res_.cpuinfo) + 1;
    const bool online = cfg_.view_online && view_.can_set_online();
    if (cfg_.view_online && !online)
        LOG_INFO("kernel view: this kernel cannot list slave CPUs in sysfs \"online\"; tools that count processors there "
                 "(the C library, older versions of top) will see only this machine's");
    view_.set_online(online);
    view_.flush();
    LOG_INFO("kernel view: active. Slave CPUs appear in /proc/cpuinfo and /proc/stat numbered from %d%s; exported processes "
             "show their slave CPU and the CPU time they use there",
             next_cpu_base_, online ? ", and count as online processors (sysfs)" : "");
}

void Master::view_slave_attached(Slave& s) {
    if (!view_.active()) return;
    view_slave_gone(s);
    auto it = phys_shift_by_uuid_.find(s.uuid);
    if (it == phys_shift_by_uuid_.end()) {
        it = phys_shift_by_uuid_.emplace(s.uuid, next_phys_shift_).first;
        next_phys_shift_ += max_physical_id(s.res.cpuinfo) + 1;
    }
    std::map<int, std::string> blocks;
    for (auto& [cpu, block] : split_cpuinfo(s.res.cpuinfo)) blocks[cpu] = std::move(block);
    const std::string& host = s.hostname.empty() ? s.name : s.hostname;
    std::vector<int> cpus = slave_cpus(s);
    for (size_t i = 0; i < cpus.size(); ++i) {
        auto b = blocks.find(cpus[i]);
        // A slave whose kernel printed nothing for this CPU still gets an entry.
        std::string block = b == blocks.end() ? "processor\t: 0\n" : b->second;
        view_.set_cpu(s.cpu_base + int(i), remote_cpuinfo_block(block, s.cpu_base + int(i), it->second, host));
    }
    s.view_cpus = int(cpus.size());
    view_.flush();
}

void Master::view_slave_gone(Slave& s) {
    if (s.view_cpus == 0) return;
    for (int i = 0; i < s.view_cpus; ++i) view_.del_cpu(s.cpu_base + i);
    s.view_cpus = 0;
    view_.flush();
}

void Master::on_view_sample(Slave& s, const ViewSampleMsg& m) {
    if (!view_.active() || s.view_cpus == 0 || m.clk_tck == 0) return;
    if (m.cpu_times.size() != m.cpus.size() * kViewStatFields || m.bodies.size() % 4 != 0) return;
    const uint64_t hz = uint64_t(view_.user_hz());
    auto local_ticks = [&](uint64_t t) { return m.clk_tck == hz ? t : t * hz / m.clk_tck; };

    std::vector<int> cpus = slave_cpus(s);
    for (size_t i = 0; i < m.cpus.size(); ++i) {
        auto pos = std::lower_bound(cpus.begin(), cpus.end(), m.cpus[i]);
        if (pos == cpus.end() || *pos != m.cpus[i] || pos - cpus.begin() >= s.view_cpus) continue;
        uint64_t v[kViewStatFields];
        for (int k = 0; k < kViewStatFields; ++k) v[k] = local_ticks(m.cpu_times[i * kViewStatFields + size_t(k)]);
        view_.cpu_stat(s.cpu_base + int(pos - cpus.begin()), v);
    }

    if (!m.bodies.empty() && exports_) {
        // Only exports that still exist: a sample taken just before an export
        // ended must not be counted towards the next one.
        std::map<uint64_t, pid_t> pid_of;
        for (const auto& info : exports_->list())
            if (info.machine == s.machine) pid_of[info.xid] = info.pid;
        for (size_t i = 0; i < m.bodies.size(); i += 4) {
            uint64_t xid = m.bodies[i];
            auto p = pid_of.find(xid);
            if (p == pid_of.end()) continue;
            auto it = view_tasks_.find(p->second);
            if (it == view_tasks_.end()) continue;
            ViewTask& t = it->second;
            if (t.xid != xid) {
                t.base_utime += t.cur_utime;
                t.base_stime += t.cur_stime;
                t.cur_utime = t.cur_stime = 0;
                t.xid = xid;
            }
            const uint64_t before = t.cur_utime + t.cur_stime;
            t.cur_utime = std::max(t.cur_utime, local_ticks(m.bodies[i + 2]));
            t.cur_stime = std::max(t.cur_stime, local_ticks(m.bodies[i + 3]));
            // Running if its body is on a CPU right now, or was for at least
            // half of the time since the last sample: the state alone is one
            // instant, and a program that makes a system call at about the
            // sampling rate could be caught waiting for it every time.
            // Otherwise the process's own state on this machine is the true
            // one (it may be executing or waiting in a system call here).
            const uint64_t used = t.cur_utime + t.cur_stime - before;
            const bool running = m.bodies[i + 1] == 'R' || used * 2 * 1000 >= uint64_t(kViewSampleMs) * hz;
            view_.set_task(p->second, t.cpu, running, t.base_utime + t.cur_utime, t.base_stime + t.cur_stime);
        }
    }
    view_.flush();
}

void Master::view_task_started(pid_t pid, int cpu) {
    if (!view_.active()) return;
    auto [it, fresh] = view_tasks_.try_emplace(pid);
    ViewTask& t = it->second;
    if (fresh) {
        // To learn when the process exits, however that happens.
        int fd = int(syscall(SYS_pidfd_open, pid, 0));
        if (fd >= 0) {
            t.pidfd = Fd(fd);
            if (!reactor_.add_fd(fd, EPOLLIN, [this, pid](uint32_t) { view_task_exited(pid); }).is_ok()) t.pidfd = Fd();
        }
    }
    t.cpu = cpu;
    view_.set_task(pid, cpu, false, t.base_utime + t.cur_utime, t.base_stime + t.cur_stime);
    view_.flush();
}

void Master::view_task_ended(pid_t pid) {
    auto it = view_tasks_.find(pid);
    if (it == view_tasks_.end()) return;
    ViewTask& t = it->second;
    t.base_utime += t.cur_utime;
    t.base_stime += t.cur_stime;
    t.cur_utime = t.cur_stime = 0;
    t.xid = 0;
    t.cpu = -1;
    if (t.pidfd.valid()) {
        // Home again: its own CPU and state, but the time it used remotely stays.
        view_.set_task(pid, -1, false, t.base_utime, t.base_stime);
    } else {
        // Without a way to notice its exit the record cannot be kept.
        view_.del_task(pid);
        view_tasks_.erase(it);
    }
    view_.flush();
}

void Master::view_task_exited(pid_t pid) {
    auto it = view_tasks_.find(pid);
    if (it == view_tasks_.end()) return;
    reactor_.del_fd(it->second.pidfd.get());
    view_tasks_.erase(it);
    view_.del_task(pid); // the kernel drops the entry of an exited task anyway
    view_.flush();
}

std::string Master::render_view() {
    if (!view_.active()) return "Kernel view: not active (slave CPUs are not shown in this machine's /proc)\n";
    int cpus = 0;
    for (const auto& [name, s] : slaves_) cpus += s->view_cpus;
    return format("Kernel view: active; %d slave CPU(s) shown in /proc/cpuinfo and /proc/stat, %zu process(es) with remote CPU time\n",
                  cpus, view_tasks_.size());
}

} // namespace dnuma
