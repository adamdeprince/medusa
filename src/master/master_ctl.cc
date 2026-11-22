// SPDX-License-Identifier: GPL-2.0-only
// dnumactl command handling. Output is rendered here, from live state.
#include <charconv>
#include <cinttypes>

#include <sys/epoll.h>

#include "common/log.h"
#include "common/util.h"
#include "master/master.h"

namespace dnuma {

namespace {
// Pulls "--name value" out of an argument list; returns false if absent.
bool take_opt(std::vector<std::string>& args, const std::string& name, std::string* value) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            *value = args[i + 1];
            args.erase(args.begin() + ssize_t(i), args.begin() + ssize_t(i) + 2);
            return true;
        }
    }
    return false;
}
bool take_flag(std::vector<std::string>& args, const std::string& name) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            args.erase(args.begin() + ssize_t(i));
            return true;
        }
    }
    return false;
}
const char* kUsage =
    "usage:\n"
    "  dnumactl slave list\n"
    "  dnumactl slave status <name>\n"
    "  dnumactl slave connect <name> [--address <host>] [--port <n>]\n"
    "  dnumactl slave disconnect <name> --drain [--timeout <duration>]\n"
    "  dnumactl slave disconnect <name> --hard [--dry-run] [--yes]\n"
    "  dnumactl slave cancel-drain <name>\n"
    "  dnumactl topology\n"
    "  dnumactl memory status [--slave <name>]\n"
    "  dnumactl process list [--slave <name>]\n"
    "  dnumactl metrics [--slave <name>]\n"
    "  dnumactl export <pid> [<slave>] [--cpu <cluster-cpu>]     move execution to a slave CPU\n"
    "  dnumactl recall <pid> | --all               bring exported processes home\n"
    "  dnumactl fault <name> <what> [<arg>]        (fault injection, for testing)\n";
} // namespace

void Master::on_ctl_accept() {
    for (;;) {
        Fd fd = accept_one(ctl_listen_.get(), nullptr);
        if (!fd.valid()) break;
        auto ctl = std::make_shared<CtlSession>();
        ctl->chan = std::make_unique<StreamChannel>(reactor_, std::move(fd), false);
        ctl_sessions_.insert(ctl);
        std::weak_ptr<CtlSession> weak = ctl;
        ctl->chan->start(
            [this, weak](Msg&& m) {
                CtlPtr ctl = weak.lock();
                CtlRequestMsg rq;
                if (!ctl || m.type != MsgType::CtlRequest || !parse_msg(m, &rq)) return;
                on_ctl_request(ctl, rq.args);
            },
            [this, weak](const Status&) {
                CtlPtr ctl = weak.lock();
                if (!ctl) return;
                ctl->finished = true; // long operations keep running; output is dropped
                reactor_.post([this, ctl] { ctl_sessions_.erase(ctl); });
            });
    }
}

void Master::on_ctl_request(CtlPtr ctl, const std::vector<std::string>& args_in) {
    std::vector<std::string> args = args_in;
    auto fail = [&](const std::string& msg) {
        ctl->out(msg + "\n");
        ctl->done(2);
    };
    if (args.empty()) return fail(kUsage);
    const std::string& cmd = args[0];

    if (cmd == "slave" && args.size() >= 2) {
        const std::string sub = args[1];
        if (sub == "list") {
            ctl->out(render_slave_list());
            return ctl->done(0);
        }
        if (args.size() < 3) return fail(kUsage);
        const std::string name = args[2];
        if (sub == "connect") {
            std::string address, port_s;
            take_opt(args, "--address", &address);
            uint16_t port = 0;
            if (take_opt(args, "--port", &port_s)) port = uint16_t(atoi(port_s.c_str()));
            return slave_connect(name, address, port, ctl);
        }
        Slave* s = slave_by_name(name);
        if (!s) return fail("unknown slave '" + name + "' (see `dnumactl slave list`)");
        if (sub == "status") {
            ctl->out(render_slave_status(*s));
            return ctl->done(0);
        }
        if (sub == "cancel-drain") {
            if (s->state != SlaveState::Draining || !s->drain) return fail(name + ": no drain in progress");
            if (s->drain->phase == DrainOp::Phase::Detach) return fail(name + ": drain is past the point where it can be cancelled");
            drain_fail(*s, Status::err(ECANCELED, "cancelled by administrator"));
            ctl->out(name + ": drain cancelled; slave is ACTIVE\n");
            return ctl->done(0);
        }
        if (sub == "disconnect") {
            bool drain = take_flag(args, "--drain"), hard = take_flag(args, "--hard");
            bool dry = take_flag(args, "--dry-run"), yes = take_flag(args, "--yes");
            std::string timeout_s;
            uint64_t timeout_ms = 0;
            if (take_opt(args, "--timeout", &timeout_s) && !parse_duration_ms(timeout_s, &timeout_ms))
                return fail("bad --timeout value '" + timeout_s + "'");
            if (drain == hard) return fail("specify exactly one of --drain or --hard");
            if (drain) return drain_start(*s, timeout_ms, ctl);
            if (!dry && !yes)
                return fail("refusing to hard-disconnect without confirmation: this kills every process that depends on " +
                            name + ".\nPreview with --dry-run, confirm with --yes.");
            return hard_disconnect(*s, dry, ctl);
        }
        return fail(kUsage);
    }
    if (cmd == "topology") {
        ctl->out(render_topology());
        return ctl->done(0);
    }
    if (cmd == "memory" && args.size() >= 2 && args[1] == "status") {
        std::string only;
        take_opt(args, "--slave", &only);
        if (!only.empty() && !slave_by_name(only)) return fail("unknown slave '" + only + "'");
        ctl->out(render_memory(only));
        return ctl->done(0);
    }
    if (cmd == "process" && args.size() >= 2 && args[1] == "list") {
        std::string only;
        take_opt(args, "--slave", &only);
        if (!only.empty() && !slave_by_name(only)) return fail("unknown slave '" + only + "'");
        ctl->out(render_processes(only));
        return ctl->done(0);
    }
    if (cmd == "metrics") {
        std::string only;
        take_opt(args, "--slave", &only);
        if (!only.empty() && !slave_by_name(only)) return fail("unknown slave '" + only + "'");
        ctl->out(render_metrics(only));
        return ctl->done(0);
    }
    if (cmd == "export" && args.size() >= 2) {
        std::string why;
        std::string cpu_arg;
        int cpu = -1;
        if (take_opt(args, "--cpu", &cpu_arg)) {
            auto [end, err] = std::from_chars(cpu_arg.data(), cpu_arg.data() + cpu_arg.size(), cpu);
            if (err != std::errc() || end != cpu_arg.data() + cpu_arg.size() || cpu < 0)
                return fail("expected a nonnegative cluster CPU number");
        }
        if (args.size() < 2 || args.size() > 3) return fail(kUsage);
        std::string name = args.size() == 3 ? args[2] : "";
        if (cpu >= 0 && name.empty()) {
            for (const auto& [n, agent] : slaves_)
                if (agent->state == SlaveState::Active && cpu >= agent->cpu_base &&
                    cpu - agent->cpu_base < agent->res.cpus_online) { name = n; break; }
            if (name.empty()) return fail("CPU is not offered by an active slave");
        }
        Slave* s = export_target(name, &why, cpu < 0);
        if (!s) return fail("cannot export: " + why);
        pid_t pid = atoi(args[1].c_str());
        Status st = export_pid(pid, *s, cpu >= 0 ? ExportPlacement::SelectCpu : ExportPlacement::SelectSlave, cpu);
        if (!st.is_ok()) return fail(format("cannot export pid %d: %s", pid, st.str().c_str()));
        ctl->out(format("pid %d is being moved to %s (see `dnumactl process list`)\n", pid, s->name.c_str()));
        return ctl->done(0);
    }
    if (cmd == "recall" && args.size() >= 2) {
        if (args[1] == "--all") {
            for (auto& [n, s] : slaves_)
                if (s->machine != kNoMachine) exports_->recall_all(s->machine, "recalled by administrator");
            ctl->out("all exported processes are coming home\n");
            return ctl->done(0);
        }
        Status st = exports_->recall(atoi(args[1].c_str()), "recalled by administrator", true);
        if (!st.is_ok()) return fail(st.message());
        ctl->out("pid " + args[1] + " is coming home\n");
        return ctl->done(0);
    }
    if (cmd == "fault" && args.size() >= 3) {
        Slave* s = slave_by_name(args[1]);
        if (!s || !s->mgmt || !lifecycle_serving(s->state)) return fail("slave '" + args[1] + "' is not attached");
        FaultInjectMsg f;
        f.what = args[2];
        if (args.size() >= 4 && !parse_size(args[3], &f.arg)) return fail("bad fault argument");
        if (f.what == "master-pull-delay") { // applies to the master's own node
            node_->set_test_pull_delay(f.arg);
            ctl->out(format("master: page pulls delayed by %" PRIu64 " ms\n", f.arg));
            return ctl->done(0);
        }
        fault_ctl_[s->name] = ctl;
        s->mgmt->send(make_msg(MsgType::FaultInject, f, 0, s->inc));
        return;
    }
    if (cmd == "version") {
        ctl->out(format("dnuma-master %s, protocol %u, kernel UAPI %s, running on %s\n", DNUMA_VERSION, kProtoVersion,
                        DNUMA_UAPI_VERSION, res_.kernel.c_str()));
        return ctl->done(0);
    }
    fail(kUsage);
}

// ---------------- rendering ----------------

std::string Master::render_slave_list() {
    std::string out = format("%-14s %-17s %-16s %-6s %-5s %-10s %-10s %s\n", "NAME", "STATE", "HOST", "NODES", "CPUS",
                             "MEMORY", "RESIDENT", "INCARNATION");
    out += format("%-14s %-17s %-16s %-6zu %-5d %-10s %-10s %s\n", (res_.hostname + "*").c_str(), "MASTER",
                  res_.hostname.c_str(), res_.nodes.size(), res_.cpus_online, human_bytes(res_.mem_total).c_str(),
                  human_bytes(coh_.usage(kMasterMachine).resident * kPageSize).c_str(), "-");
    for (auto& [name, s] : slaves_) {
        bool known = !s->res.nodes.empty();
        out += format("%-14s %-17s %-16s %-6s %-5s %-10s %-10s %" PRIu64 "\n", name.c_str(), to_string(s->state),
                      s->hostname.empty() ? "-" : s->hostname.c_str(),
                      known ? std::to_string(s->res.nodes.size()).c_str() : "-",
                      known ? std::to_string(s->res.cpus_online).c_str() : "-",
                      known ? human_bytes(s->res.mem_total).c_str() : "-",
                      s->machine == kNoMachine ? "-" : human_bytes(coh_.usage(s->machine).resident * kPageSize).c_str(),
                      s->inc);
    }
    if (slaves_.empty()) out += "(no slaves known; attach one with `dnumactl slave connect <name>`)\n";
    return out;
}

std::string Master::render_slave_status(Slave& s) {
    std::string out;
    if (s.state == SlaveState::Draining && s.drain) out += drain_progress(s) + "\n";
    out += format("Slave:        %s\nState:        %s\n", s.name.c_str(), to_string(s.state));
    if (s.state == SlaveState::Connecting) out += format("Stage:        %s\n", s.attach_stage.c_str());
    out += format("Identity:     uuid %s, machine id %d, hostname %s\n", s.uuid.empty() ? "(unknown)" : s.uuid.c_str(),
                  s.machine == kNoMachine ? -1 : int(s.machine), s.hostname.empty() ? "(unknown)" : s.hostname.c_str());
    out += format("Incarnation:  %" PRIu64 " (attached %" PRIu64 " time(s) since master start)\n", s.inc, s.attaches);
    if (s.state == SlaveState::FenceIncomplete) {
        uint64_t now = now_ns();
        out += format("Fencing:      INCOMPLETE - the remote machine is not proven stopped; lease expires in %.1f s\n",
                      s.fence_safe_ns > now ? double(s.fence_safe_ns - now) / 1e9 : 0.0);
    } else if (s.state == SlaveState::Disconnected && s.attaches) {
        out += format("Fence proof:  %s\n", to_string(s.proof));
    }
    if (!s.last_error.empty()) out += format("Last error:   %s\n", s.last_error.c_str());
    if (!s.killed.empty()) {
        out += "Processes terminated by the last disconnect:\n";
        for (const auto& k : s.killed) out += "    " + k + "\n";
    }
    if (s.res.nodes.empty()) return out;

    UffdCaps uc = uffd_caps_from_features(s.res.uffd_features, s.res.uffd_dev);
    if (s.state == SlaveState::Active && s.export_block.empty())
        out += format("Cluster CPUs: %d-%d\n", s.cpu_base, s.cpu_base + s.res.cpus_online - 1);
    out += format("Kernel:       %s\nCPU:          %s, %d socket(s), %d CPUs\nMemory:       %s total, %s available\n",
                  s.res.kernel.c_str(), s.res.cpu_model.c_str(), s.res.sockets, s.res.cpus_online,
                  human_bytes(s.res.mem_total).c_str(), human_bytes(s.res.mem_available).c_str());
    out += format("Huge pages:   %" PRIu64 " x %s (%" PRIu64 " free)\nuserfaultfd:  %s\n", s.res.hugepages_total,
                  human_bytes(s.res.hugepage_size).c_str(), s.res.hugepages_free, uc.summary().c_str());
    out += format("XSAVE:        xcr0=0x%" PRIx64 ", %u-byte state area\n", s.res.xcr0, s.res.xsave_size);
    if (lifecycle_serving(s.state)) {
        PeerLink* pl = links_.get(s.machine);
        uint64_t now = now_ns();
        out += format("Lease:        %u ms, last renewed %.2f s ago\n", cfg_.lease_ms, double(now - s.last_renew_ns) / 1e9);
        out += format("RDMA link:    %s:%u via %s, %s; pool %s, capacity offered %s\n", s.rdma_ip.c_str(), s.rdma_port,
                      pl && pl->link ? pl->link->device().c_str() : "?", pl && pl->link && pl->link->is_up() ? "up" : "DOWN",
                      human_bytes(s.pool_bytes).c_str(), human_bytes(s.capacity_pages * kPageSize).c_str());
        if (pl && pl->link) {
            const auto& st = pl->link->stats();
            out += format("              self-test WRITE %s / READ %s; %s reads (%s), %s messages out, %s in, %s op errors\n",
                          format_ns(pl->verify_write_ns).c_str(), format_ns(pl->verify_read_ns).c_str(),
                          with_commas(st.reads).c_str(), human_bytes(st.read_bytes).c_str(), with_commas(st.msgs_tx).c_str(),
                          with_commas(st.msgs_rx).c_str(), with_commas(st.op_errors).c_str());
        }
        MachineUsage u = coh_.usage(s.machine);
        uint64_t ctxs = 0, images = 0;
        for (auto& [id, p] : procs_) {
            images += p.images.count(s.machine);
            for (auto& [cid, c] : p.ctxs) ctxs += (c.machine == s.machine && c.state != ExecCtx::State::Exited) ? 1 : 0;
        }
        out += format("Usage:        %s pages resident (%s), %s owned, %s dirty; %" PRIu64 " execution context(s), %" PRIu64 " image(s)\n",
                      with_commas(u.resident).c_str(), human_bytes(u.resident * kPageSize).c_str(), with_commas(u.owned).c_str(),
                      with_commas(u.exclusive).c_str(), ctxs, images);
        if (s.report_ns)
            out += format("Agent report: pool %s/%s slots in use, %u image(s), %u context(s) (%.1f s old)\n",
                          with_commas(s.report.pool_slots_used).c_str(), with_commas(s.report.pool_slots_total).c_str(),
                          s.report.images, s.report.contexts, double(now - s.report_ns) / 1e9);
    }
    out += "NUMA domains:\n";
    for (const auto& n : s.res.nodes) {
        int logical = -1;
        for (const auto& a : s.nodes)
            if (a.phys_index == n.phys_index) logical = a.logical;
        out += format("    logical %-3d physical %-2d %3zu CPUs [%s]  %s total, %s free\n", logical, n.phys_index, n.cpus.size(),
                      format_cpulist(n.cpus).c_str(), human_bytes(n.mem_total).c_str(), human_bytes(n.mem_free).c_str());
    }
    out += "RDMA ports:\n";
    for (const auto& p : s.res.rdma_ports)
        out += format("    %s/%d %-10s %-10s %-24s NUMA %d  pci %s  %s %s%s\n", p.device.c_str(), p.port, p.link_layer.c_str(),
                      p.state.c_str(), p.rate.c_str(), p.numa_node, p.pci.c_str(), p.netdev.c_str(), p.ipv4.c_str(),
                      p.ipv4 == s.rdma_ip && !p.ipv4.empty() ? "  <- in use" : "");
    out += "Lifecycle history (most recent last):\n";
    for (const auto& h : s.history) out += "    " + h + "\n";
    return out;
}

std::string Master::render_topology() {
    std::string out = format("Logical NUMA topology: %zu node(s) present, %zu known\n\n", topo_.present_count(), topo_.all().size());
    out += format("%-8s %-7s %-14s %-9s %-5s %-11s %-11s %-10s %-6s %s\n", "LOGICAL", "NODE-ID", "MACHINE", "PHYSICAL",
                  "CPUS", "MEM-TOTAL", "MEM-FREE", "RESIDENT", "CTXS", "STATE");
    for (const NodeRecord& r : topo_.all()) {
        uint64_t ctxs = 0;
        for (auto& [id, p] : procs_)
            for (auto& [cid, c] : p.ctxs) ctxs += (c.node == r.id && c.state != ExecCtx::State::Exited) ? 1 : 0;
        std::string resident = "-";
        const char* state = "MASTER";
        if (r.machine == kMasterMachine) {
            std::vector<uint64_t> v = node_->resident_by_node();
            size_t idx = 0;
            for (size_t i = 0; i < res_.nodes.size(); ++i)
                if (res_.nodes[i].phys_index == r.phys_index) idx = i;
            if (idx < v.size()) resident = human_bytes(v[idx] * kPageSize);
        } else {
            Slave* s = nullptr;
            for (auto& [n, sl] : slaves_)
                if (sl->machine == r.machine) s = sl.get();
            state = !r.present ? "ABSENT" : (s ? to_string(s->state) : "?");
            if (s && r.present) {
                size_t idx = 0;
                for (size_t i = 0; i < s->res.nodes.size(); ++i)
                    if (s->res.nodes[i].phys_index == r.phys_index) idx = i;
                if (idx < s->report.node_resident.size()) resident = human_bytes(s->report.node_resident[idx] * kPageSize);
            }
        }
        out += format("%-8d %-7u %-14s %-9d %-5zu %-11s %-11s %-10s %-6" PRIu64 " %s\n", r.logical, r.id,
                      machine_name(r.machine).c_str(), r.phys_index, r.cpus.size(), human_bytes(r.mem_total).c_str(),
                      r.present ? human_bytes(r.mem_free).c_str() : "-", resident.c_str(), ctxs, state);
    }
    out += "\nRDMA placement (NUMA domain physically closest to each HCA port in use):\n";
    const RdmaPortInfo* mp = choose_rdma_port(res_, "");
    if (mp) out += format("    %-14s %s/%d %s %s -> physical NUMA %d\n", res_.hostname.c_str(), mp->device.c_str(), mp->port,
                          mp->rate.c_str(), mp->ipv4.c_str(), mp->numa_node);
    for (auto& [name, s] : slaves_)
        for (const auto& p : s->res.rdma_ports)
            if (!p.ipv4.empty() && p.ipv4 == s->rdma_ip)
                out += format("    %-14s %s/%d %s %s -> physical NUMA %d\n", name.c_str(), p.device.c_str(), p.port, p.rate.c_str(),
                              p.ipv4.c_str(), p.numa_node);
    return out;
}

std::string Master::render_memory(const std::string& only) {
    std::string out = format("%-14s %-12s %-12s %-12s %-12s %-12s %-10s %s\n", "MACHINE", "CAPACITY", "RESIDENT", "OWNED",
                             "DIRTY", "IN-FLIGHT", "POOL-USED", "STATE");
    auto row = [&](const std::string& name, MachineId m, const char* state, const std::string& pool) {
        MachineUsage u = coh_.usage(m);
        out += format("%-14s %-12s %-12s %-12s %-12s %-12s %-10s %s\n", name.c_str(), human_bytes(capacity_pages(m) * kPageSize).c_str(),
                      human_bytes(u.resident * kPageSize).c_str(), human_bytes(u.owned * kPageSize).c_str(),
                      human_bytes(u.exclusive * kPageSize).c_str(), human_bytes(coh_.inflight_to(m) * kPageSize).c_str(),
                      pool.c_str(), state);
    };
    if (only.empty())
        row(res_.hostname + "*", kMasterMachine, "MASTER",
            format("%s/%s", with_commas(pool_.slots_used()).c_str(), with_commas(pool_.slots_total()).c_str()));
    for (auto& [name, s] : slaves_) {
        if (!only.empty() && name != only) continue;
        if (s->machine == kNoMachine) continue;
        row(name, s->machine, to_string(s->state),
            s->report_ns ? format("%s/%s", with_commas(s->report.pool_slots_used).c_str(), with_commas(s->report.pool_slots_total).c_str()) : "-");
    }
    out += format("\nPage transactions in progress: %zu\n", coh_.active_transactions());
    out += "\nPer process:\n";
    out += format("    %-8s %-22s %-12s", "PID", "NAME", "ALLOCATED");
    std::vector<std::pair<std::string, MachineId>> cols;
    if (only.empty()) cols.emplace_back(res_.hostname, kMasterMachine);
    for (auto& [name, s] : slaves_)
        if ((only.empty() || name == only) && s->machine != kNoMachine) cols.emplace_back(name, s->machine);
    for (auto& [name, m] : cols) out += format(" %-14s", ("on " + name).c_str());
    out += "\n";
    bool any = false;
    for (auto& [id, p] : procs_) {
        if (!only.empty() && coh_.usage(id, cols.empty() ? kNoMachine : cols[0].second).resident == 0) continue;
        any = true;
        out += format("    %-8d %-22s %-12s", p.home_pid, p.name.c_str(), human_bytes(coh_.allocated_pages(id) * kPageSize).c_str());
        for (auto& [name, m] : cols) out += format(" %-14s", human_bytes(coh_.usage(id, m).resident * kPageSize).c_str());
        out += "\n";
    }
    if (!any) out += "    (none)\n";
    return out;
}

std::string Master::render_processes(const std::string& only) {
    MachineId filter = kNoMachine;
    if (!only.empty()) filter = slave_by_name(only)->machine;
    std::string out = format("%-6s %-8s %-22s %-9s %-11s %-11s %s\n", "DPID", "PID", "NAME", "CONTEXTS", "ALLOCATED",
                             "RESIDENT", "PLACEMENT (images / contexts / memory)");
    bool any = false;
    for (auto& [id, p] : procs_) {
        if (filter != kNoMachine) {
            bool dep = coh_.usage(id, filter).resident > 0 || p.images.count(filter);
            for (auto& [cid, c] : p.ctxs) dep = dep || c.machine == filter;
            if (!dep) continue;
        }
        any = true;
        uint64_t resident = 0, live = 0;
        std::string place;
        std::set<MachineId> machines{kMasterMachine};
        for (auto& [m, st] : p.images) machines.insert(m);
        for (MachineId m = 0; m < next_machine_; ++m)
            if (coh_.usage(id, m).resident) machines.insert(m);
        for (auto& [cid, c] : p.ctxs) live += c.state != ExecCtx::State::Exited ? 1 : 0;
        for (MachineId m : machines) {
            uint64_t ctxs = 0;
            for (auto& [cid, c] : p.ctxs) ctxs += (c.machine == m && c.state != ExecCtx::State::Exited) ? 1 : 0;
            uint64_t r = coh_.usage(id, m).resident;
            resident += r;
            place += format("%s%s[%s%" PRIu64 " ctx, %s]", place.empty() ? "" : "  ", machine_name(m).c_str(),
                            p.images.count(m) ? "image, " : "", ctxs, human_bytes(r * kPageSize).c_str());
        }
        out += format("%-6" PRIu64 " %-8d %-22s %-9" PRIu64 " %-11s %-11s %s\n", id, p.home_pid, p.name.c_str(), live,
                      human_bytes(coh_.allocated_pages(id) * kPageSize).c_str(), human_bytes(resident * kPageSize).c_str(), place.c_str());
        for (auto& [cid, c] : p.ctxs) {
            if (filter != kNoMachine && c.machine != filter) continue;
            static const char* names[] = {"creating", "running", "suspending", "moving", "exited"};
            out += format("         ctx %-5" PRIu64 " %-10s on %s node %d cpu %d tid %d, %u migration(s)\n", cid, names[int(c.state)],
                          machine_name(c.machine).c_str(), c.info.logical_node, c.info.cpu, c.info.tid, c.info.migrations);
        }
    }
    if (!any) out += "(no distributed processes)\n";
    if (only.empty() || filter != kNoMachine) out += render_exports();
    if (!kill_log_.empty() && only.empty()) {
        out += "\nRecent forced terminations:\n";
        for (const auto& k : kill_log_) out += "    " + k + "\n";
    }
    return out;
}

std::string Master::render_metrics(const std::string& only) {
    std::string out;
    if (only.empty()) {
        uint64_t attached = 0, remote_ctx = 0;
        for (auto& [n, s] : slaves_) attached += lifecycle_serving(s->state) ? 1 : 0;
        for (auto& [id, p] : procs_)
            for (auto& [cid, c] : p.ctxs) remote_ctx += (c.machine != kMasterMachine && c.state != ExecCtx::State::Exited) ? 1 : 0;
        out += "== summary ==\n";
        out += format("  %-40s %" PRIu64 "\n  %-40s %zu\n  %-40s %" PRIu64 "\n  %-40s %zu\n", "attached_slaves", attached,
                      "available_numa_nodes", topo_.present_count(), "running_remote_execution_contexts", remote_ctx,
                      "distributed_processes", procs_.size());
        for (MachineId m = 0; m < next_machine_; ++m) {
            MachineUsage u = coh_.usage(m);
            if (m != kMasterMachine && !slave_by_machine(m)) continue;
            out += format("  %-40s resident=%s owned=%s dirty=%s\n", ("pages[" + machine_name(m) + "]").c_str(),
                          with_commas(u.resident).c_str(), with_commas(u.owned).c_str(), with_commas(u.exclusive).c_str());
        }
        Metrics coord = metrics_;
        if (exports_) exports_->merge_metrics(&coord);
        out += "== coordinator ==\n" + coord.render("  ");
        out += "== node " + res_.hostname + " (master) ==\n" + node_metrics_.render("  ");
        for (const auto& [m, pl] : links_.peers()) {
            if (!pl->link) continue;
            const auto& st = pl->link->stats();
            out += format("  %-40s reads=%s read_bytes=%s writes=%s msgs_tx=%s msgs_rx=%s errors=%s\n",
                          ("rdma_link[" + machine_name(m) + "]").c_str(), with_commas(st.reads).c_str(), human_bytes(st.read_bytes).c_str(),
                          with_commas(st.writes).c_str(), with_commas(st.msgs_tx).c_str(), with_commas(st.msgs_rx).c_str(),
                          with_commas(st.op_errors).c_str());
            out += format("  %-40s avg=%s max=%s waits-for-credits=%s waits-for-buffers=%s\n", ("rdma_send_queue[" + machine_name(m) + "]").c_str(),
                          format_ns(st.msgs_tx ? st.send_wait_total_ns / st.msgs_tx : 0).c_str(), format_ns(st.send_wait_max_ns).c_str(),
                          with_commas(st.credit_stalls).c_str(), with_commas(st.buffer_stalls).c_str());
        }
    }
    for (auto& [name, s] : slaves_) {
        if (!only.empty() && name != only) continue;
        if (!s->report_ns) continue;
        out += format("== node %s (slave, report %.1f s old) ==\n", name.c_str(), double(now_ns() - s->report_ns) / 1e9);
        out += s->report.metrics.render("  ");
    }
    // Bandwidth is derived from what was actually transferred and timed.
    auto bw = [&](const Metrics& mt, const std::string& who) {
        const Histogram* h = mt.hist("rdma_read_ns");
        uint64_t bytes = mt.get("rdma_read_bytes");
        if (h && h->sum() && bytes)
            out += format("  %-40s >= %.1f MiB/s over %s single-page reads (bytes / summed per-read latency; overlapping reads make this a lower bound)\n", ("rdma_read_rate[" + who + "]").c_str(),
                          double(bytes) / (double(h->sum()) / 1e9) / 1048576.0, with_commas(h->count()).c_str());
    };
    out += "== derived ==\n";
    if (only.empty()) bw(node_metrics_, res_.hostname);
    for (auto& [name, s] : slaves_)
        if ((only.empty() || name == only) && s->report_ns) bw(s->report.metrics, name);
    return out;
}

} // namespace dnuma
