// SPDX-License-Identifier: GPL-2.0-only
// Slave lifecycle: attach, lease, failure detection, fencing, draining.
#include <algorithm>
#include <cinttypes>

#include "common/log.h"
#include "common/util.h"
#include "master/master.h"
#include "transport/auth.h"

namespace dnuma {

namespace {
constexpr uint64_t kMs = 1000000ull;
// The agent measures its lease on its own clock from the moment it *sent*
// the renewal; we count from when we answered, and add a margin for clock
// rate differences and scheduling delay before treating it as expired.
uint64_t lease_safe_ns(uint32_t lease_ms) { return uint64_t(lease_ms) * kMs * 5 / 4; }
} // namespace

void Master::drop_mgmt(Slave& s) {
    if (!s.mgmt) return;
    s.mgmt->close();
    // Destroy outside whatever handler we may be running in.
    reactor_.post([ch = std::shared_ptr<StreamChannel>(std::move(s.mgmt))] {});
}

void Master::teardown_slave_links(Slave& s) {
    if (s.machine != kNoMachine) {
        // Destroying the link destroys its queue pair, protection domain and
        // every memory registration: all RDMA access the slave had is gone.
        links_.drop(s.machine);
        PeerMsg pf;
        pf.peer = s.machine;
        pf.peer_inc = s.inc;
        for (auto& [name, o] : slaves_)
            if (o.get() != &s && o->mgmt && lifecycle_serving(o->state))
                o->mgmt->send(make_msg(MsgType::PeerFence, pf, 0, o->inc));
    }
    s.rdma_ready = false;
    reactor_.cancel_timer(s.lease_timer);
    s.lease_timer = 0;
}

// ============================ attach ============================

void Master::slave_connect(const std::string& name, const std::string& address, uint16_t port, CtlPtr ctl) {
    Slave* s = slave_by_name(name);
    if (!s) {
        auto ns = std::make_unique<Slave>();
        ns->name = name;
        s = ns.get();
        slaves_[name] = std::move(ns);
    }
    switch (s->state) {
    case SlaveState::Active:
        ctl->out(format("%s: already attached (ACTIVE, incarnation %" PRIu64 "); nothing to do\n", name.c_str(), s->inc));
        return ctl->done(0);
    case SlaveState::Connecting:
        ctl->out(format("%s: a connect is already in progress (%s)\n", name.c_str(), s->attach_stage.c_str()));
        return ctl->done(0);
    case SlaveState::Draining:
    case SlaveState::Failed:
    case SlaveState::Fencing:
        ctl->out(format("%s: cannot connect while %s\n", name.c_str(), to_string(s->state)));
        return ctl->done(1);
    case SlaveState::Disconnected:
    case SlaveState::FenceIncomplete:
        break;
    }
    if (!address.empty()) s->address = address;
    if (port) s->port = port;
    s->inc = ++next_inc_; // a fresh incarnation for every attachment
    transition(*s, LifecycleEvent::Connect, "administrative connect");
    s->attach_ctl = ctl;
    s->attach_ns = now_ns();
    s->attach_stage = "contacting agent";
    s->last_error.clear();
    s->last_renew_ns = 0;
    s->rdma_ready = false;
    s->peers_pending.clear();
    Incarnation inc = s->inc;
    s->attach_timer = reactor_.add_timer(cfg_.attach_timeout_ms * kMs, [this, name, inc] {
        Slave* s = slave_by_name(name);
        if (s && s->inc == inc && s->state == SlaveState::Connecting)
            attach_fail(*s, Status::err(ETIMEDOUT, "attach timed out while %s", s->attach_stage.c_str()));
    });

    std::string host = s->address.empty() ? name : s->address;
    uint16_t p = s->port ? s->port : cfg_.agent_port;
    tcp_connect_async(reactor_, host, p, 5000, [this, name, inc](Status st, Fd fd, std::string ip) {
        Slave* s = slave_by_name(name);
        if (!s || s->inc != inc || s->state != SlaveState::Connecting) return;
        if (!st.is_ok()) return attach_fail(*s, st);
        s->peer_ip = ip;
        s->attach_stage = "authenticating";
        s->mgmt = std::make_unique<StreamChannel>(reactor_, std::move(fd), false);
        s->mgmt->start(
            [this, name, inc](Msg&& m) {
                Slave* s = slave_by_name(name);
                if (s && s->inc == inc) on_mgmt_msg(*s, std::move(m));
            },
            [this, name, inc](const Status& st) { on_mgmt_closed(name, inc, st); });
        HelloMsg h;
        h.master_instance = instance_;
        h.nonce.resize(32);
        if (!random_bytes(h.nonce.data(), h.nonce.size()).is_ok())
            return attach_fail(*s, Status::err(EIO, "no entropy for authentication"));
        s->nonce_m = h.nonce;
        s->mgmt->send(make_msg(MsgType::Hello, h));
    });
}

void Master::attach_fail(Slave& s, const Status& st) {
    if (s.state != SlaveState::Connecting) return;
    LOG_WARN("slave %s: attach failed: %s", s.name.c_str(), st.str().c_str());
    s.last_error = st.str();
    reactor_.cancel_timer(s.attach_timer);
    teardown_slave_links(s);
    drop_mgmt(s);
    if (s.machine != kNoMachine) topo_.detach(s.machine);
    transition(s, LifecycleEvent::ConnectFail, st.message());
    if (CtlPtr ctl = std::move(s.attach_ctl)) {
        ctl->out(format("%s: connect failed: %s\n", s.name.c_str(), st.str().c_str()));
        if (s.state == SlaveState::FenceIncomplete)
            ctl->out(format("%s remains FENCE_INCOMPLETE: its previous incarnation is still not proven stopped\n", s.name.c_str()));
        ctl->done(1);
    }
}

void Master::on_mgmt_closed(const std::string& name, Incarnation inc, const Status& st) {
    Slave* s = slave_by_name(name);
    if (!s || s->inc != inc) return;
    drop_mgmt(*s);
    switch (s->state) {
    case SlaveState::Connecting:
        attach_fail(*s, Status::err(ECONNRESET, "management connection closed while %s: %s", s->attach_stage.c_str(), st.message().c_str()));
        break;
    case SlaveState::Active:
    case SlaveState::Draining:
        slave_failed(*s, "management connection lost: " + st.str());
        break;
    case SlaveState::Fencing:
        fence_finish(*s, false, "connection closed before the agent acknowledged fencing");
        break;
    default:
        break;
    }
}

void Master::on_mgmt_msg(Slave& s, Msg&& m) {
    switch (m.type) {
    case MsgType::HelloAck:
        return on_hello_ack(s, m);
    case MsgType::NodeAttachAck:
        return on_attach_ack(s, m);
    case MsgType::NodeAssignAck:
        if (s.state == SlaveState::Connecting) attach_rdma(s);
        return;
    case MsgType::Heartbeat:
        return on_heartbeat(s, m);
    case MsgType::ResourceReport: {
        ResourceReportMsg r;
        if (!parse_msg(m, &r)) return;
        s.report = std::move(r);
        s.report_ns = now_ns();
        topo_.update_free(s.machine, s.report.node_mem_free);
        return;
    }
    case MsgType::ViewSample: {
        ViewSampleMsg v;
        if (parse_msg(m, &v)) on_view_sample(s, v);
        return;
    }
    case MsgType::PeerStatus: {
        PeerMsg p;
        if (!parse_msg(m, &p) || s.state != SlaveState::Connecting) return;
        if (p.status != 0)
            return attach_fail(s, Status::err(p.status, "RDMA link to peer %s failed: %s", machine_name(p.peer).c_str(), p.text.c_str()));
        s.peers_pending.erase(p.peer);
        return attach_maybe_complete(s);
    }
    case MsgType::NodeFenceAck: {
        SimpleMsg a;
        if (!parse_msg(m, &a) || s.state != SlaveState::Fencing) return;
        fence_finish(s, true, format("agent destroyed %" PRIu64 " image(s) and %" PRIu64 " context(s)", a.a, a.b));
        return;
    }
    case MsgType::NodeDetachAck: {
        SimpleMsg a;
        if (!parse_msg(m, &a) || s.state != SlaveState::Draining || !s.drain) return;
        if (a.a != 0 || a.b != 0)
            LOG_WARN("slave %s: detach found %" PRIu64 " leftover image(s), %" PRIu64 " context(s)", s.name.c_str(), a.a, a.b);
        return drain_finish(s);
    }
    case MsgType::FaultInjectAck: {
        SimpleMsg a;
        auto it = fault_ctl_.find(s.name);
        if (!parse_msg(m, &a) || it == fault_ctl_.end()) return;
        it->second->out(format("%s: %s\n", s.name.c_str(), a.text.c_str()));
        it->second->done(a.status ? 1 : 0);
        fault_ctl_.erase(it);
        return;
    }
    default:
        LOG_WARN("slave %s: unexpected %s on management channel", s.name.c_str(), to_string(m.type));
    }
}

void Master::on_hello_ack(Slave& s, const Msg& m) {
    if (s.state != SlaveState::Connecting) return;
    HelloAckMsg a;
    if (!parse_msg(m, &a)) return attach_fail(s, Status::err(EPROTO, "malformed HELLO_ACK"));
    if (a.status != 0) return attach_fail(s, Status::err(a.status, "agent refused: %s", a.error.c_str()));
    if (a.version < kProtoVersionMin || a.version > kProtoVersion)
        return attach_fail(s, Status::err(EPROTONOSUPPORT, "agent speaks protocol version %u", a.version));
    if (!proof_equal(a.mac, agent_proof(key_, s.nonce_m, a.nonce, a.uuid)))
        return attach_fail(s, Status::err(EACCES, "agent failed authentication (cluster key mismatch?)"));
    if (a.uuid.empty()) return attach_fail(s, Status::err(EPROTO, "agent has no identity"));

    // Identity is the agent's UUID, not the name or address it was reached by.
    for (auto& [name, o] : slaves_) {
        if (o.get() != &s && o->uuid == a.uuid && o->state != SlaveState::Disconnected)
            return attach_fail(s, Status::err(EEXIST, "this agent (uuid %s) is already known as slave '%s' (%s)",
                                              a.uuid.c_str(), name.c_str(), to_string(o->state)));
    }
    auto it = uuid_machine_.find(a.uuid);
    MachineId machine = it != uuid_machine_.end() ? it->second : next_machine_++;
    uuid_machine_[a.uuid] = machine;
    s.machine = machine;
    s.uuid = a.uuid;
    s.hostname = a.hostname;

    AuthMsg auth;
    auth.mac = master_proof(key_, s.nonce_m, a.nonce, instance_);
    s.mgmt->send(make_msg(MsgType::Auth, auth));
    s.mgmt->enable_mac(session_key(key_, s.nonce_m, a.nonce));

    s.attach_stage = "attaching node";
    NodeAttachMsg na;
    na.machine = machine;
    na.incarnation = s.inc;
    na.master_instance = instance_;
    na.lease_ms = cfg_.lease_ms;
    na.renew_ms = cfg_.renew_ms;
    na.master_hostname = res_.hostname;
    na.view_ms = view_.active() ? kViewSampleMs : 0;
    s.mgmt->send(make_msg(MsgType::NodeAttach, na, 0, s.inc));
}

void Master::on_attach_ack(Slave& s, const Msg& m) {
    if (s.state != SlaveState::Connecting) return;
    NodeAttachAckMsg a;
    if (!parse_msg(m, &a)) return attach_fail(s, Status::err(EPROTO, "malformed NODE_ATTACH_ACK"));
    if (a.status != 0) return attach_fail(s, Status::err(a.status, "agent could not attach: %s", a.error.c_str()));
    if (a.rdma_ip.empty() || a.rdma_port == 0)
        return attach_fail(s, Status::err(ENODEV, "agent reported no usable RDMA endpoint"));
    s.res = std::move(a.resources);
    s.rdma_ip = a.rdma_ip;
    s.rdma_port = a.rdma_port;
    s.pool_bytes = a.pool_bytes;
    s.capacity_pages = a.capacity_bytes / kPageSize;
    s.stale_images = a.stale_images_destroyed;
    s.stale_contexts = a.stale_contexts_destroyed;
    if (s.fence_pending) {
        // The agent has just destroyed (and counted) everything belonging to
        // earlier incarnations: that is the proof the old fence lacked.
        LOG_INFO("slave %s: reattach handshake completed the pending fence (%u stale image(s), %u context(s) destroyed)",
                 s.name.c_str(), s.stale_images, s.stale_contexts);
        s.fence_pending = false;
        s.proof = FenceProof::Reattached;
        reactor_.cancel_timer(s.fence_timer);
    }
    s.nodes = topo_.attach(s.machine, s.uuid, s.res);
    NodeAssignMsg assign;
    assign.nodes = s.nodes;
    s.attach_stage = "assigning NUMA node identities";
    s.mgmt->send(make_msg(MsgType::NodeAssign, assign, 0, s.inc));
}

void Master::attach_rdma(Slave& s) {
    s.attach_stage = "establishing RDMA link";
    std::string name = s.name;
    Incarnation inc = s.inc;
    MachineId machine = s.machine;
    // The master's side of a link is identified by its instance id.
    std::vector<uint8_t> ticket = make_link_ticket(key_, 0, kMasterMachine, instance_, machine, inc);
    rdma_.connect(s.rdma_ip, s.rdma_port, ticket, 8000,
                  [this, name, inc, machine](Status st, std::unique_ptr<RdmaLink> link, std::vector<uint8_t> reply) {
        Slave* s = slave_by_name(name);
        if (!s || s->inc != inc || s->state != SlaveState::Connecting) return;
        if (!st.is_ok()) return attach_fail(*s, st);
        LinkTicket t;
        if (!check_link_ticket(key_, reply, 1, &t) || t.src != machine || t.src_inc != inc ||
            t.dst != kMasterMachine || t.dst_inc != instance_)
            return attach_fail(*s, Status::err(EACCES, "agent failed RDMA link authentication"));
        s->attach_stage = "exchanging memory registrations";
        links_.add(machine, inc, instance_, std::move(link), [this, name, inc, machine](Status st) {
            Slave* s = slave_by_name(name);
            if (!s || s->inc != inc || s->state != SlaveState::Connecting) return;
            if (!st.is_ok()) return attach_fail(*s, st);
            s->attach_stage = "verifying RDMA READ/WRITE";
            links_.verify(machine, [this, name, inc, machine](Status st) {
                Slave* s = slave_by_name(name);
                if (!s || s->inc != inc || s->state != SlaveState::Connecting) return;
                if (!st.is_ok()) return attach_fail(*s, st);
                // Now have the agent prove the opposite direction.
                s->attach_stage = "waiting for agent-side RDMA verification";
                send_to_machine(machine, make_msg(MsgType::LinkVerify, SimpleMsg{}));
            });
        });
    });
}

void Master::on_link_verify_ack(Slave& s, const SimpleMsg& ack) {
    if (s.state != SlaveState::Connecting) return;
    if (ack.status != 0)
        return attach_fail(s, Status::err(ack.status, "agent-side RDMA verification failed: %s", ack.text.c_str()));
    s.rdma_ready = true;
    attach_peers(s);
}

void Master::attach_peers(Slave& s) {
    // Bulk data moves directly between the machines involved, so a new slave
    // gets a link to every slave that is already serving.
    for (auto& [name, o] : slaves_) {
        if (o.get() == &s || !lifecycle_serving(o->state) || !o->mgmt) continue;
        PeerMsg expect;
        expect.peer = s.machine;
        expect.peer_inc = s.inc;
        o->mgmt->send(make_msg(MsgType::PeerExpect, expect, 0, o->inc));
        PeerMsg conn;
        conn.peer = o->machine;
        conn.peer_inc = o->inc;
        conn.rdma_ip = o->rdma_ip;
        conn.rdma_port = o->rdma_port;
        s.mgmt->send(make_msg(MsgType::PeerConnect, conn, 0, s.inc));
        s.peers_pending.insert(o->machine);
    }
    if (!s.peers_pending.empty()) s.attach_stage = "linking to peer slaves";
    attach_maybe_complete(s);
}

void Master::attach_maybe_complete(Slave& s) {
    if (s.state != SlaveState::Connecting || !s.rdma_ready || !s.peers_pending.empty()) return;
    if (s.last_renew_ns == 0) {
        s.attach_stage = "waiting for the first lease renewal";
        return;
    }
    reactor_.cancel_timer(s.attach_timer);
    ++s.attaches;
    s.ever_leased = true;
    s.killed.clear();
    transition(s, LifecycleEvent::ConnectOk, "attach complete");
    export_slave_attached(s);
    metrics_.add("slave_attaches");
    metrics_.observe("slave_attach_ns", now_ns() - s.attach_ns);
    PeerLink* pl = links_.get(s.machine);
    if (CtlPtr ctl = std::move(s.attach_ctl)) {
        std::string nodes;
        for (const auto& n : s.nodes) nodes += format("%s%d", nodes.empty() ? "" : ",", n.logical);
        ctl->out(format("%s: ACTIVE (host %s, uuid %s, incarnation %" PRIu64 ")\n"
                        "  resources: %d sockets, %d CPUs, %zu NUMA nodes, %s RAM; logical NUMA nodes %s\n"
                        "  RDMA: %s via %s, pool %s; verified WRITE %s / READ %s of %zu bytes in both directions\n",
                        s.name.c_str(), s.hostname.c_str(), s.uuid.c_str(), s.inc, s.res.sockets, s.res.cpus_online,
                        s.res.nodes.size(), human_bytes(s.res.mem_total).c_str(), nodes.c_str(), s.rdma_ip.c_str(),
                        pl && pl->link ? pl->link->device().c_str() : "?", human_bytes(s.pool_bytes).c_str(),
                        pl ? format_ns(pl->verify_write_ns).c_str() : "?", pl ? format_ns(pl->verify_read_ns).c_str() : "?",
                        kVerifyBytes));
        if (s.stale_images || s.stale_contexts)
            ctl->out(format("  agent destroyed %u stale image(s) and %u stale context(s) from an earlier incarnation\n",
                            s.stale_images, s.stale_contexts));
        ctl->done(0);
    }
}

// ============================ lease ============================

void Master::on_heartbeat(Slave& s, const Msg& m) {
    HeartbeatMsg hb;
    if (!parse_msg(m, &hb) || !s.mgmt) return;
    HeartbeatAckMsg ack;
    ack.seq = hb.seq;
    bool grant = lifecycle_serving(s.state) || s.state == SlaveState::Connecting;
    if (grant) {
        ack.lease_ms = cfg_.lease_ms;
        s.last_renew_ns = now_ns();
        s.fence_safe_ns = s.last_renew_ns + lease_safe_ns(cfg_.lease_ms);
        arm_lease_timer(s);
    }
    s.mgmt->send(make_msg(MsgType::HeartbeatAck, ack, 0, s.inc));
    if (grant && s.state == SlaveState::Connecting) attach_maybe_complete(s);
}

void Master::arm_lease_timer(Slave& s) {
    reactor_.cancel_timer(s.lease_timer);
    std::string name = s.name;
    Incarnation inc = s.inc;
    s.lease_timer = reactor_.add_timer(uint64_t(cfg_.lease_ms) * kMs, [this, name, inc] {
        Slave* s = slave_by_name(name);
        if (!s || s->inc != inc || !lifecycle_serving(s->state)) return;
        uint64_t silent = (now_ns() - s->last_renew_ns) / kMs;
        if (silent >= cfg_.lease_ms)
            slave_failed(*s, format("lease expired: no heartbeat for %" PRIu64 " ms", silent));
    });
}

// ============================ failure and fencing ============================

void Master::slave_failed(Slave& s, const std::string& reason) {
    if (s.state == SlaveState::Connecting) return attach_fail(s, Status::err(ECONNRESET, "%s", reason.c_str()));
    if (!lifecycle_serving(s.state)) return;
    LOG_ERROR("slave %s FAILED: %s", s.name.c_str(), reason.c_str());
    s.last_error = reason;
    metrics_.add("slave_failures");
    transition(s, LifecycleEvent::Failure, reason);
    transition(s, LifecycleEvent::FenceBegin, "isolating failed slave");
    begin_fence(s, "failed: " + reason, nullptr);
}

std::vector<ProcId> Master::dependents_of(MachineId m) {
    // Conservative rule: any page residency (even a replica) or any execution
    // context on the machine makes the process dependent on it.
    std::vector<ProcId> out;
    for (auto& [id, p] : procs_) {
        bool dep = coh_.usage(id, m).resident > 0;
        for (auto& [cid, c] : p.ctxs)
            if (c.machine == m && c.state != ExecCtx::State::Exited) dep = true;
        if (dep) out.push_back(id);
    }
    return out;
}

void Master::hard_disconnect(Slave& s, bool dry_run, CtlPtr ctl) {
    if (s.state == SlaveState::Disconnected) {
        ctl->out(format("%s: already DISCONNECTED; nothing to do\n", s.name.c_str()));
        return ctl->done(0);
    }
    if (s.state == SlaveState::Fencing || s.state == SlaveState::Failed || s.state == SlaveState::FenceIncomplete) {
        ctl->out(render_slave_status(s));
        return ctl->done(s.state == SlaveState::FenceIncomplete ? 3 : 0);
    }
    std::vector<ProcId> deps = s.machine == kNoMachine ? std::vector<ProcId>{} : dependents_of(s.machine);
    if (dry_run) {
        std::string out = format("Hard disconnect: %s (dry run, nothing changed)\n\nAffected processes:\n", s.name.c_str());
        uint64_t total_pages = 0;
        for (ProcId id : deps) {
            Process& p = procs_[id];
            uint64_t on_slave = coh_.usage(id, s.machine).resident;
            uint64_t ctxs = 0;
            for (auto& [cid, c] : p.ctxs) ctxs += (c.machine == s.machine && c.state != ExecCtx::State::Exited) ? 1 : 0;
            out += format("    PID %-7d %-24s %s on %s, %" PRIu64 " execution context(s) there\n", p.home_pid, p.name.c_str(),
                          human_bytes(on_slave * kPageSize).c_str(), s.name.c_str(), ctxs);
            for (MachineId m = 0; m < next_machine_; ++m) total_pages += coh_.usage(id, m).owned;
        }
        std::vector<std::string> exported = exports_ ? exports_->describe(s.machine) : std::vector<std::string>{};
        for (const std::string& e : exported) out += "    " + e + "\n";
        if (deps.empty() && exported.empty()) out += "    (none)\n";
        out += format("\nProcesses to terminate: %zu\nMemory to discard:      %s (%s resident on %s)\n\n"
                      "Unaffected processes will continue.\n",
                      deps.size() + exported.size(), human_bytes(total_pages * kPageSize).c_str(),
                      human_bytes(coh_.usage(s.machine).resident * kPageSize).c_str(), s.name.c_str());
        ctl->out(out);
        return ctl->done(0);
    }
    if (s.state == SlaveState::Connecting) {
        attach_fail(s, Status::err(ECANCELED, "cancelled by hard disconnect"));
        ctl->out(format("%s: pending connect cancelled\n", s.name.c_str()));
        return ctl->done(0);
    }
    transition(s, LifecycleEvent::HardDisconnect, "administrative hard disconnect");
    metrics_.add("slave_hard_disconnects");
    begin_fence(s, "hard-disconnected by administrator", ctl);
}

void Master::begin_fence(Slave& s, const std::string& cause, CtlPtr ctl) {
    // Precondition: s.state == Fencing, so nothing new is placed on or sent
    // to the slave (machine_usable() is already false).
    s.fence_ctl = ctl;
    MachineId m = s.machine;
    if (s.drain) {
        if (CtlPtr dc = s.drain->ctl) {
            dc->out(format("%s: drain abandoned: slave is being fenced (%s)\n", s.name.c_str(), cause.c_str()));
            dc->done(1);
        }
        *s.drain->alive = false;
        reactor_.cancel_timer(s.drain->deadline_timer);
        reactor_.cancel_timer(s.drain->progress_timer);
        reactor_.cancel_timer(s.drain->detach_timer);
        s.drain.reset();
    }

    // Terminate every dependent process (SIGKILL semantics), which also
    // cancels its pending faults and reclaims its directory metadata.
    s.killed.clear();
    for (ProcId id : dependents_of(m)) {
        Process& p = procs_[id];
        s.killed.push_back(format("PID %d  %s", p.home_pid, p.name.c_str()));
        destroy_process(id, format("slave %s %s", s.name.c_str(), cause.c_str()), true);
    }
    // Native processes whose execution was on the slave have lost the state
    // that lived there.
    if (exports_) exports_->machine_lost(m, cause, &s.killed);
    // Survivors: abort transfers that were heading to the slave (the source
    // keeps the page) and forget idle images there.
    coh_.machine_gone(m);
    for (auto& [id, p] : procs_) {
        p.images.erase(m);
        auto w = p.image_waiters.find(m);
        if (w != p.image_waiters.end()) {
            auto waiters = std::move(w->second);
            p.image_waiters.erase(w);
            for (auto& fn : waiters) fn(Status::err(ENODEV, "slave %s is gone", s.name.c_str()));
        }
    }
    teardown_slave_links(s);
    topo_.detach(m);

    if (s.mgmt && s.mgmt->is_open()) {
        SimpleMsg f;
        f.text = cause;
        s.mgmt->send(make_msg(MsgType::NodeFence, f, 0, s.inc));
        std::string name = s.name;
        Incarnation inc = s.inc;
        // Never wait indefinitely for a slave that may be dead.
        s.fence_timer = reactor_.add_timer(cfg_.fence_ack_timeout_ms * kMs, [this, name, inc] {
            Slave* s = slave_by_name(name);
            if (s && s->inc == inc && s->state == SlaveState::Fencing)
                fence_finish(*s, false, "agent did not acknowledge fencing in time");
        });
    } else {
        fence_finish(s, false, "agent unreachable");
    }
}

void Master::fence_finish(Slave& s, bool confirmed, const std::string& how) {
    if (s.state != SlaveState::Fencing) return;
    reactor_.cancel_timer(s.fence_timer);
    s.fence_timer = 0;
    drop_mgmt(s);
    std::string verdict;
    int code = 0;
    if (confirmed) {
        s.proof = FenceProof::AgentConfirmed;
        s.fence_pending = false;
        transition(s, LifecycleEvent::FenceConfirmed, how);
        verdict = "Fencing: CONFIRMED by agent (" + how + ")";
    } else if (!s.ever_leased) {
        // No lease was ever granted to this slave, so it never ran anything.
        s.proof = FenceProof::None;
        transition(s, LifecycleEvent::FenceConfirmed, "no lease was ever granted");
        verdict = "Fencing: not required (slave never held a lease)";
    } else {
        // Our side is isolated, but the remote machine may still be running
        // stale contexts. Say so; do not pretend otherwise.
        s.fence_pending = true;
        s.proof = FenceProof::None;
        transition(s, LifecycleEvent::FenceUnconfirmed, how);
        uint64_t now = now_ns();
        uint64_t wait = s.fence_safe_ns > now ? s.fence_safe_ns - now : 0;
        verdict = format("Fencing: INCOMPLETE (%s). The remote machine is not proven stopped; its lease runs out in %.1f s. "
                         "Its resources will not be reused until then or until it reattaches.",
                         how.c_str(), double(wait) / 1e9);
        code = 3;
        std::string name = s.name;
        Incarnation inc = s.inc;
        s.fence_timer = reactor_.add_timer(wait + kMs, [this, name, inc] {
            Slave* s = slave_by_name(name);
            if (!s || s->inc != inc || s->state != SlaveState::FenceIncomplete || !s->fence_pending) return;
            // The agent's lease has run out on any sane clock: a live agent
            // has self-fenced, and images die with their agent.
            s->fence_pending = false;
            s->proof = FenceProof::LeaseExpired;
            transition(*s, LifecycleEvent::LeaseExpired, "lease of the old incarnation expired");
        });
    }
    if (CtlPtr ctl = std::move(s.fence_ctl)) {
        std::string out = format("Hard disconnect: %s\n\nTerminated processes:\n", s.name.c_str());
        for (const std::string& k : s.killed) out += "    " + k + "\n";
        if (s.killed.empty()) out += "    (none)\n";
        out += format("\nProcesses terminated: %zu\nState: %s\n%s\nUnaffected processes continue.\n", s.killed.size(),
                      to_string(s.state), verdict.c_str());
        ctl->out(out);
        ctl->done(code);
    }
}

// ============================ draining ============================

NodeId Master::pick_node_on(MachineId m) {
    const NodeRecord* best = nullptr;
    for (const NodeRecord* r : topo_.nodes_of(m))
        if (!best || r->mem_free > best->mem_free) best = r;
    return best ? best->id : kNoNode;
}

MachineId Master::pick_evacuation_target(MachineId avoid, uint64_t* budget) {
    // The master first, then any other slave that is ACTIVE.
    std::vector<MachineId> cands{kMasterMachine};
    for (auto& [name, o] : slaves_)
        if (o->machine != avoid && o->state == SlaveState::Active) cands.push_back(o->machine);
    for (MachineId c : cands) {
        uint64_t f = free_pages(c);
        if (f > 0) {
            if (budget) *budget = f;
            return c;
        }
    }
    return kNoMachine;
}

std::string Master::drain_progress(Slave& s) {
    MachineId m = s.machine;
    uint64_t ctxs = 0;
    for (auto& [id, p] : procs_)
        for (auto& [cid, c] : p.ctxs) ctxs += (c.machine == m && c.state != ExecCtx::State::Exited) ? 1 : 0;
    MachineUsage u = coh_.usage(m);
    return format("%s: %s\n\n"
                  "Exported native processes:  %zu\n"
                  "Running execution contexts: %s\n"
                  "Pages remaining:            %s\n"
                  "Dirty pages remaining:      %s\n"
                  "Memory to evacuate:         %s\n"
                  "Outstanding RDMA transfers: %zu\n",
                  s.name.c_str(), to_string(s.state), exports_ ? exports_->count(m) : size_t(0), with_commas(ctxs).c_str(), with_commas(u.resident).c_str(),
                  with_commas(u.exclusive).c_str(), human_bytes(u.exclusive * kPageSize).c_str(),
                  coh_.transactions_involving(m));
}

void Master::drain_start(Slave& s, uint64_t timeout_ms, CtlPtr ctl) {
    if (s.state == SlaveState::Disconnected) {
        ctl->out(format("%s: already DISCONNECTED; nothing to do\n", s.name.c_str()));
        return ctl->done(0);
    }
    if (s.state == SlaveState::Draining) {
        ctl->out(format("%s: a drain is already in progress\n\n%s", s.name.c_str(), drain_progress(s).c_str()));
        return ctl->done(1);
    }
    if (s.state != SlaveState::Active) {
        ctl->out(format("%s: cannot drain while %s\n", s.name.c_str(), to_string(s.state)));
        return ctl->done(1);
    }
    transition(s, LifecycleEvent::Drain, "administrative drain");
    metrics_.add("slave_drains");
    s.drain = std::make_unique<DrainOp>();
    DrainOp& d = *s.drain;
    d.ctl = ctl;
    d.t0 = now_ns();
    std::string name = s.name;
    Incarnation inc = s.inc;
    if (timeout_ms) {
        d.deadline_ns = d.t0 + timeout_ms * kMs;
        d.deadline_timer = reactor_.add_timer(timeout_ms * kMs, [this, name, inc, timeout_ms] {
            Slave* s = slave_by_name(name);
            if (!s || s->inc != inc || s->state != SlaveState::Draining || !s->drain) return;
            drain_fail(*s, Status::err(ETIMEDOUT, "drain did not complete within %" PRIu64 " s", timeout_ms / 1000));
        });
    }
    d.progress_timer = reactor_.add_timer(1000 * kMs, [this, name, inc] {
        Slave* s = slave_by_name(name);
        if (s && s->inc == inc && s->state == SlaveState::Draining && s->drain && s->drain->ctl)
            s->drain->ctl->out(drain_progress(*s) + "\n");
    }, 1000 * kMs);
    if (s.mgmt) s.mgmt->send(make_msg(MsgType::NodeDrain, SimpleMsg{}, 0, s.inc));
    ctl->out(drain_progress(s) + "\n");

    // Fail early, before moving anything, if the data cannot fit elsewhere.
    uint64_t need = coh_.usage(s.machine).exclusive, avail = free_pages(kMasterMachine);
    for (auto& [n, o] : slaves_)
        if (o.get() != &s && o->state == SlaveState::Active) avail += free_pages(o->machine);
    if (need > avail)
        return drain_fail(s, Status::err(ENOMEM, "insufficient memory elsewhere: %s must be evacuated but only %s is available on the remaining machines",
                                         human_bytes(need * kPageSize).c_str(), human_bytes(avail * kPageSize).c_str()));
    drain_step(s.machine);
}

void Master::drain_step(MachineId m) {
    Slave* sp = slave_by_machine(m);
    if (!sp || !sp->drain || sp->state != SlaveState::Draining) return;
    Slave& s = *sp;
    DrainOp& d = *s.drain;

    if (d.phase == DrainOp::Phase::Exec) {
        // Move execution away first, so nothing on the slave keeps faulting
        // pages back while they are being evacuated.
        uint64_t remaining = exports_ ? exports_->count(m) : 0;
        if (remaining) exports_->recall_all(m, "slave is being drained"); // always possible: they just come home
        NodeId dest = pick_node_on(kMasterMachine);
        for (auto& [id, p] : procs_) {
            for (auto& [cid, c] : p.ctxs) {
                if (c.machine != m || c.state == ExecCtx::State::Exited) continue;
                ++remaining;
                if (c.state == ExecCtx::State::Running) (void)request_ctx_migration(p, c, dest);
            }
        }
        if (remaining) return; // re-entered when a context moves, exits or suspends
        d.phase = DrainOp::Phase::Pages;
        coh_.for_each_page_on(m, [&](ProcId proc, PageNo page, const PageEntry&) { d.work.emplace_back(proc, page); });
        d.last_resident = coh_.usage(m).resident;
    }
    if (d.phase == DrainOp::Phase::Pages) return drain_pages(s);

    if (d.phase == DrainOp::Phase::Images) {
        uint64_t images = 0;
        for (auto& [id, p] : procs_) {
            auto it = p.images.find(m);
            if (it == p.images.end()) continue;
            ++images;
            if (it->second == Process::ImageState::Ready) {
                it->second = Process::ImageState::Destroying;
                ImageMsg im;
                im.proc = id;
                send_to_machine(m, make_msg(MsgType::ImageDestroy, im));
            }
        }
        if (images) return; // re-entered on IMAGE_EXITED
        // Final verification before letting go of the slave.
        MachineUsage u = coh_.usage(m);
        if (u.resident != 0 || !dependents_of(m).empty() || coh_.transactions_involving(m) != 0)
            return drain_fail(s, Status::err(EBUSY, "verification failed: %" PRIu64 " page(s), %zu dependent process(es), %zu transfer(s) still involve the slave",
                                             u.resident, dependents_of(m).size(), coh_.transactions_involving(m)));
        d.phase = DrainOp::Phase::Detach;
        if (!s.mgmt) return slave_failed(s, "management connection missing at detach");
        s.mgmt->send(make_msg(MsgType::NodeDetach, SimpleMsg{}, 0, s.inc));
        std::string name = s.name;
        Incarnation inc = s.inc;
        d.detach_timer = reactor_.add_timer(5000 * kMs, [this, name, inc] {
            Slave* s = slave_by_name(name);
            if (s && s->inc == inc && s->state == SlaveState::Draining)
                slave_failed(*s, "agent did not acknowledge detach after a completed drain");
        });
    }
}

void Master::drain_pages(Slave& s) {
    DrainOp& d = *s.drain;
    if (d.pumping) return;
    d.pumping = true;
    MachineId m = s.machine;
    std::string name = s.name;
    Incarnation inc = s.inc;
    std::shared_ptr<bool> alive = d.alive;
    auto done_cb = [this, name, inc, alive](uint64_t DrainOp::*counter) {
        return [this, name, inc, alive, counter](Status st) {
            if (!*alive) return;
            Slave* s = slave_by_name(name);
            if (!s || s->inc != inc || !s->drain) return;
            DrainOp& d = *s->drain;
            --d.inflight;
            if (st.is_ok()) ++(d.*counter);
            else if (st.code() != ECANCELED && d.error.is_ok()) d.error = st; // ECANCELED: its process went away
            drain_pages(*s);
        };
    };

    while (d.inflight < cfg_.drain_window && !d.work.empty() && d.error.is_ok()) {
        auto [proc, page] = d.work.front();
        d.work.pop_front();
        const PageEntry* e = coh_.lookup(proc, page);
        if (!e || (e->state != PageState::Exclusive && e->state != PageState::Shared)) continue;
        if (e->owner == m) {
            MachineId dest = kNoMachine;
            bool handoff = false;
            for (MachineId r : e->readers)
                if (machine_usable(r) && r != m) {
                    dest = r; // a replica already exists there: ownership moves without data
                    handoff = true;
                    break;
                }
            if (dest == kNoMachine) dest = pick_evacuation_target(m, nullptr);
            if (dest == kNoMachine) {
                d.error = Status::err(ENOMEM, "insufficient memory elsewhere: no remaining machine has capacity for page 0x%" PRIx64 " of process %" PRIu64,
                                      addr_of(page), proc);
                break;
            }
            ++d.inflight;
            coh_.migrate(proc, page, dest, done_cb(handoff ? &DrainOp::handed_off : &DrainOp::moved));
        } else if (std::find(e->readers.begin(), e->readers.end(), m) != e->readers.end()) {
            ++d.inflight;
            coh_.drop_replica(proc, page, m, done_cb(&DrainOp::dropped));
        }
    }
    d.pumping = false;

    if (!d.error.is_ok()) {
        if (d.inflight == 0) drain_fail(s, d.error);
        return; // otherwise wait for in-flight transfers to settle first
    }
    if (!d.work.empty() || d.inflight != 0) return;

    uint64_t resident = coh_.usage(m).resident;
    if (resident != 0) {
        // Something still lives on the slave (e.g. it was busy when first
        // visited). Rescan, but insist on progress.
        if (d.rescans > 0 && resident >= d.last_resident)
            return drain_fail(s, Status::err(EBUSY, "%" PRIu64 " page(s) remain on the slave and cannot be moved", resident));
        d.last_resident = resident;
        ++d.rescans;
        coh_.for_each_page_on(m, [&](ProcId proc, PageNo page, const PageEntry&) { d.work.emplace_back(proc, page); });
        reactor_.post([this, name, inc] {
            Slave* s = slave_by_name(name);
            if (s && s->inc == inc && s->drain && s->state == SlaveState::Draining) drain_pages(*s);
        });
        return;
    }

    // No page is left. Allocations whose placement target was on this slave
    // are re-homed so that nothing references its NUMA nodes any more.
    NodeId new_home = pick_node_on(kMasterMachine);
    for (ProcId proc : coh_.processes()) {
        std::vector<PageNo> rehome;
        coh_.for_each_region(proc, [&](const Region& rg) {
            const NodeRecord* rec = topo_.by_id(rg.home);
            if (rec && rec->machine == m) rehome.push_back(rg.first);
        });
        for (PageNo first : rehome) coh_.set_region_home(proc, first, new_home);
    }
    d.phase = DrainOp::Phase::Images;
    drain_step(m);
}

void Master::drain_fail(Slave& s, const Status& st) {
    if (!s.drain || s.state != SlaveState::Draining) return;
    DrainOp& d = *s.drain;
    // Describe precisely what is in the way.
    std::string blocker;
    if (d.phase == DrainOp::Phase::Exec) {
        std::string ids;
        uint64_t n = 0;
        for (auto& [id, p] : procs_)
            for (auto& [cid, c] : p.ctxs)
                if (c.machine == s.machine && c.state != ExecCtx::State::Exited) {
                    ids += format("%s%" PRIu64 " (PID %d %s)", n ? ", " : "", cid, p.home_pid, p.name.c_str());
                    ++n;
                }
        blocker = format("%" PRIu64 " execution context(s) have not reached a migration point or exited: %s", n, ids.c_str());
    } else if (d.phase == DrainOp::Phase::Pages) {
        MachineUsage u = coh_.usage(s.machine);
        blocker = format("%s page(s) (%s) still resident on the slave, %u transfer(s) in flight",
                         with_commas(u.resident).c_str(), human_bytes(u.resident * kPageSize).c_str(), d.inflight);
    } else if (d.phase == DrainOp::Phase::Images) {
        blocker = "waiting for idle images on the slave to exit";
    } else {
        blocker = "waiting for the agent to acknowledge detach";
    }
    LOG_WARN("slave %s: drain failed: %s; blocker: %s", s.name.c_str(), st.str().c_str(), blocker.c_str());
    s.last_error = "drain failed: " + st.message();
    reactor_.cancel_timer(d.deadline_timer);
    reactor_.cancel_timer(d.progress_timer);
    reactor_.cancel_timer(d.detach_timer);
    *d.alive = false; // late completions of in-flight transfers are ignored
    CtlPtr ctl = d.ctl;
    uint64_t moved = d.moved, handed = d.handed_off, dropped = d.dropped;
    s.drain.reset();
    metrics_.add("slave_drain_failures");
    // A failed drain is never turned into a hard disconnect.
    transition(s, LifecycleEvent::DrainCancel, st.message());
    if (ctl) {
        ctl->out(format("\n%s: drain FAILED: %s\nBlocker: %s\n"
                        "%s remains attached and ACTIVE. No process was terminated and no page was discarded\n"
                        "(%s page(s) already evacuated stay where they are; %s replica(s) were dropped).\n",
                        s.name.c_str(), st.str().c_str(), blocker.c_str(), s.name.c_str(),
                        with_commas(moved + handed).c_str(), with_commas(dropped).c_str()));
        ctl->done(1);
    }
}

void Master::drain_finish(Slave& s) {
    if (!s.drain || s.state != SlaveState::Draining) return;
    DrainOp& d = *s.drain;
    reactor_.cancel_timer(d.deadline_timer);
    reactor_.cancel_timer(d.progress_timer);
    reactor_.cancel_timer(d.detach_timer);
    CtlPtr ctl = d.ctl;
    uint64_t moved = d.moved, handed = d.handed_off, dropped = d.dropped;
    double secs = double(now_ns() - d.t0) / 1e9;
    s.drain.reset();
    teardown_slave_links(s);
    drop_mgmt(s);
    topo_.detach(s.machine);
    s.proof = FenceProof::CleanDetach;
    s.fence_pending = false;
    transition(s, LifecycleEvent::DrainDone, "evacuation verified, agent detached");
    metrics_.observe("slave_drain_ns", uint64_t(secs * 1e9));
    if (ctl) {
        ctl->out(format("\n%s: DETACHED after graceful drain (%.2f s)\n"
                        "  pages transferred: %s (%s), ownership handed to existing replicas: %s, replicas dropped: %s\n"
                        "  all dependent processes are still running\n",
                        s.name.c_str(), secs, with_commas(moved).c_str(), human_bytes(moved * kPageSize).c_str(),
                        with_commas(handed).c_str(), with_commas(dropped).c_str()));
        ctl->done(0);
    }
}

} // namespace dnuma
