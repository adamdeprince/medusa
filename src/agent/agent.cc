// SPDX-License-Identifier: GPL-2.0-only
#include "agent/agent.h"

#include "common/workers.h"

#include <cinttypes>
#include <set>

#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"
#include "transport/auth.h"

namespace dnuma {

namespace {
constexpr uint64_t kMs = 1000000ull;
}

Agent::Agent(Reactor& reactor, AgentConfig cfg)
    : reactor_(reactor), cfg_(std::move(cfg)), rdma_(reactor), links_(kNoMachine, pool_, metrics_) {}

Agent::~Agent() { shutdown(); }

void Agent::shutdown() {
    self_fence("agent shutting down");
    bodies_.reset();
    conns_.clear();
    master_ = nullptr;
    links_.drop_all();
    node_.reset();
}

Status Agent::init() {
    DNUMA_TRY(load_cluster_key(cfg_.key_file, &key_));

    // Stable identity: generated once, kept across restarts and reboots, and
    // independent of the hostname.
    if (::mkdir(cfg_.state_dir.c_str(), 0700) != 0 && errno != EEXIST)
        return Status::sys("mkdir %s", cfg_.state_dir.c_str());
    std::string id_path = cfg_.state_dir + "/agent-uuid";
    uuid_ = read_attr(id_path);
    if (uuid_.empty()) {
        uuid_ = make_uuid();
        DNUMA_TRY(write_file_atomic(id_path, uuid_ + "\n", 0600));
    }

    DNUMA_TRY(probe_machine(&res_, cfg_.nodes));
    if (!cfg_.nodes.empty()) {
        // Before any thread or child exists, so that all of them inherit it.
        std::vector<int> phys, cpus;
        for (const auto& n : res_.nodes) {
            phys.push_back(n.phys_index);
            cpus.insert(cpus.end(), n.cpus.begin(), n.cpus.end());
        }
        DNUMA_TRY(confine_to_nodes(phys, cpus));
    }
    const RdmaPortInfo* port = choose_rdma_port(res_, cfg_.rdma_ip);
    if (!port)
        return Status::err(ENODEV, "no usable RDMA port%s%s (need an ACTIVE port with an IPv4 address)",
                           cfg_.rdma_ip.empty() ? "" : " with address ", cfg_.rdma_ip.c_str());
    rdma_ip_ = port->ipv4;
    capacity_bytes_ = cfg_.capacity_bytes ? cfg_.capacity_bytes : res_.mem_available / 2;

    std::vector<int> phys;
    for (const auto& n : res_.nodes) phys.push_back(n.phys_index);
    DNUMA_TRY(pool_.init(cfg_.pool_bytes, phys));
    DNUMA_TRY(rdma_.init());
    DNUMA_TRY(rdma_.listen(rdma_ip_, cfg_.rdma_port,
                           [this](const RdmaEnv::Incoming& in, std::vector<uint8_t>* reply, RdmaEnv::LinkReady* ready) {
                               return on_rdma_request(in, reply, ready);
                           },
                           &rdma_port_));

    NodeConfig nc;
    nc.self = kNoMachine; // assigned at attach
    nc.is_master = false;
    nc.hostname = res_.hostname;
    NodeCore::Hooks hooks;
    hooks.to_coordinator = [this](Msg m) {
        if (inc_ == 0) return;
        m.inc = inc_;
        links_.send(kMasterMachine, m);
    };
    node_ = std::make_unique<NodeCore>(reactor_, nc, pool_, links_, metrics_, std::move(hooks));
    node_->set_resources(res_);

    std::string stub = cfg_.body_stub;
    if (stub.empty()) {
        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        std::string dir = n > 0 ? std::string(self, size_t(n)) : std::string(".");
        stub = dir.substr(0, dir.rfind('/')) + "/dnuma-body";
    }
    if (access(stub.c_str(), X_OK) != 0) LOG_WARN("body stub %s not found: this slave cannot host exported processes", stub.c_str());
    unsigned workers = cfg_.body_workers ? cfg_.body_workers : WorkerPool::default_threads(unsigned(res_.cpus_online));
    bodies_ = std::make_unique<BodyShards>(reactor_, stub, [this](Msg m) {
        if (inc_ == 0) return;
        m.inc = inc_;
        links_.send(kMasterMachine, m);
    }, workers);

    links_.set_handlers([this](MachineId from, Msg&& m) { on_ops_msg(from, std::move(m)); },
                        [this](MachineId peer, Incarnation, const Status& st) {
                            if (partitioned_) {
                                LOG_WARN("link to machine %u went down during simulated partition: %s", peer, st.str().c_str());
                                return;
                            }
                            if (peer == kMasterMachine) self_fence("RDMA link to master lost: " + st.str());
                            else LOG_WARN("RDMA link to peer machine %u lost: %s", peer, st.str().c_str());
                        });

    DNUMA_TRY(tcp_listen(cfg_.listen_addr, cfg_.port, &listen_, nullptr));
    DNUMA_TRY(reactor_.add_fd(listen_.get(), EPOLLIN, [this](uint32_t) { on_accept(); }));

    UffdCaps uc = uffd_caps_from_features(res_.uffd_features, res_.uffd_dev);
    LOG_INFO("dnuma-agent %s (kernel UAPI %s) on %s, kernel %s, identity %s", DNUMA_VERSION, DNUMA_UAPI_VERSION,
             res_.hostname.c_str(), res_.kernel.c_str(), uuid_.c_str());
    LOG_INFO("%d sockets, %d CPUs, %zu NUMA nodes, %s RAM; userfaultfd: %s", res_.sockets, res_.cpus_online,
             res_.nodes.size(), human_bytes(res_.mem_total).c_str(), uc.summary().c_str());
    if (!cfg_.nodes.empty()) {
        std::vector<int> cpus;
        for (const auto& n : res_.nodes) cpus.insert(cpus.end(), n.cpus.begin(), n.cpus.end());
        std::sort(cpus.begin(), cpus.end());
        LOG_INFO("offering only NUMA node(s) %s: this agent and everything it hosts are confined to CPUs %s and to those nodes' memory",
                 format_cpulist(cfg_.nodes).c_str(), format_cpulist(cpus).c_str());
    }
    LOG_INFO("management on %s:%u; RDMA on %s:%u via %s/%d (%s, NUMA node %d); pool %s, offering %s",
             cfg_.listen_addr.empty() ? "*" : cfg_.listen_addr.c_str(), cfg_.port, rdma_ip_.c_str(), rdma_port_,
             port->device.c_str(), port->port, port->rate.c_str(), port->numa_node, human_bytes(pool_.bytes()).c_str(),
             human_bytes(capacity_bytes_).c_str());
    return Status::ok();
}

// ============================ management channel ============================

void Agent::on_accept() {
    for (;;) {
        std::string peer;
        Fd fd = accept_one(listen_.get(), &peer);
        if (!fd.valid()) break;
        auto conn = std::make_unique<Conn>();
        Conn* c = conn.get();
        c->peer = peer;
        c->chan = std::make_unique<StreamChannel>(reactor_, std::move(fd), false);
        conns_[c] = std::move(conn);
        c->chan->start([this, c](Msg&& m) {
            if (conns_.count(c)) on_conn_msg(c, std::move(m));
        }, [this, c](const Status& st) { on_conn_closed(c, st); });
        LOG_DEBUG("management connection from %s", peer.c_str());
    }
}

void Agent::close_conn(Conn* c) {
    auto it = conns_.find(c);
    if (it == conns_.end()) return;
    if (master_ == c) master_ = nullptr;
    it->second->chan->close();
    // Destroyed from the reactor, never from inside its own handler.
    reactor_.post([dead = std::shared_ptr<Conn>(std::move(it->second))] {});
    conns_.erase(it);
}

void Agent::on_conn_closed(Conn* c, const Status& st) {
    if (!conns_.count(c)) return;
    bool was_master = master_ == c;
    close_conn(c);
    // Losing the master means nobody can vouch for this incarnation any more.
    if (was_master && !partitioned_) self_fence("management connection to master lost: " + st.message());
}

void Agent::on_conn_msg(Conn* c, Msg&& m) {
    if (partitioned_) return; // simulated partition: nothing gets through
    if (!c->authed) {
        if (m.type == MsgType::Hello) return on_hello(c, m);
        if (m.type == MsgType::Auth) return on_auth(c, m);
        LOG_WARN("unauthenticated %s from %s; closing", to_string(m.type), c->peer.c_str());
        return close_conn(c);
    }
    if (m.type == MsgType::NodeAttach) return on_attach(c, m);
    if (m.type == MsgType::NodeFence && m.inc != inc_) {
        // The master is fencing an incarnation this agent has already
        // destroyed (it holds state for at most one incarnation at a time).
        SimpleMsg ack;
        ack.text = "incarnation was already destroyed";
        if (m.inc == last_fenced_inc_) { // report what that fence actually destroyed
            ack.a = last_fenced_images_;
            ack.b = last_fenced_contexts_;
        }
        if (c->chan && c->chan->is_open()) c->chan->send(make_msg(MsgType::NodeFenceAck, ack, 0, m.inc));
        return;
    }
    // Everything else must come from the connection that owns the current
    // incarnation, and name that incarnation.
    if (c != master_ || m.inc != inc_ || inc_ == 0) {
        LOG_WARN("ignoring %s from %s: not the current incarnation", to_string(m.type), c->peer.c_str());
        return;
    }
    switch (m.type) {
    case MsgType::NodeAssign: {
        NodeAssignMsg a;
        if (!parse_msg(m, &a)) return;
        node_->set_node_map(a.nodes);
        std::string nums;
        for (const auto& n : a.nodes) nums += format("%s%d->%d", nums.empty() ? "" : " ", n.phys_index, n.logical);
        LOG_INFO("NUMA node identities assigned (physical->logical): %s", nums.c_str());
        return reply(c, MsgType::NodeAssignAck, SimpleMsg{});
    }
    case MsgType::HeartbeatAck: {
        HeartbeatAckMsg a;
        if (!parse_msg(m, &a)) return;
        auto it = hb_sent_.find(a.seq);
        if (it == hb_sent_.end()) return;
        uint64_t sent = it->second;
        hb_sent_.erase(hb_sent_.begin(), std::next(it));
        metrics_.observe("heartbeat_rtt_ns", now_ns() - sent);
        // The lease runs from when the renewal was *sent*: that instant is
        // certainly before the master answered, so the master's view of the
        // expiry can never be earlier than ours.
        if (a.lease_ms) arm_lease(sent + uint64_t(a.lease_ms) * kMs);
        return;
    }
    case MsgType::NodeDrain:
        LOG_INFO("master is draining this node: no new work will arrive");
        return;
    case MsgType::NodeFence: {
        SimpleMsg f;
        (void)parse_msg(m, &f);
        uint32_t images = 0, contexts = 0;
        Incarnation inc = inc_;
        self_fence("fenced by master: " + f.text, &images, &contexts);
        SimpleMsg ack;
        ack.a = images;
        ack.b = contexts;
        ack.text = "all images and contexts of the incarnation destroyed";
        if (c->chan && c->chan->is_open()) c->chan->send(make_msg(MsgType::NodeFenceAck, ack, 0, inc));
        return;
    }
    case MsgType::NodeDetach: {
        uint32_t images = 0, contexts = 0;
        Incarnation inc = inc_;
        // Acknowledge before the RDMA link goes away, so the master sees an
        // orderly detach rather than a link failure.
        self_fence("detached by master after drain", &images, &contexts, true);
        SimpleMsg ack;
        ack.a = images;
        ack.b = contexts;
        if (c->chan && c->chan->is_open()) c->chan->send(make_msg(MsgType::NodeDetachAck, ack, 0, inc));
        reactor_.add_timer(200 * kMs, [this] {
            if (inc_ == 0) links_.drop_all();
        });
        return;
    }
    case MsgType::PeerExpect: {
        PeerMsg p;
        if (parse_msg(m, &p)) expected_peers_[p.peer] = p.peer_inc;
        return;
    }
    case MsgType::PeerConnect: {
        PeerMsg p;
        if (parse_msg(m, &p)) peer_connect(p);
        return;
    }
    case MsgType::PeerFence: {
        PeerMsg p;
        if (!parse_msg(m, &p)) return;
        expected_peers_.erase(p.peer);
        links_.drop(p.peer); // revokes everything that peer could reach here
        LOG_INFO("peer machine %u fenced: link and registrations destroyed", p.peer);
        return;
    }
    case MsgType::FaultInject:
        return on_fault(c, m);
    default:
        LOG_WARN("unexpected %s on management channel", to_string(m.type));
    }
}

void Agent::on_hello(Conn* c, const Msg& m) {
    HelloMsg h;
    HelloAckMsg a;
    if (!parse_msg(m, &h) || h.nonce.size() < 16) return close_conn(c);
    if (h.ver_min > kProtoVersion || h.ver_max < kProtoVersionMin) {
        a.status = EPROTONOSUPPORT;
        a.error = format("agent speaks protocol %u..%u", kProtoVersionMin, kProtoVersion);
        c->chan->send(make_msg(MsgType::HelloAck, a));
        return;
    }
    c->nonce_m = h.nonce;
    c->nonce_a.resize(32);
    if (!random_bytes(c->nonce_a.data(), c->nonce_a.size()).is_ok()) return close_conn(c);
    c->master_instance = h.master_instance;
    c->hello = true;
    a.version = std::min(h.ver_max, kProtoVersion);
    a.uuid = uuid_;
    a.hostname = res_.hostname;
    a.nonce = c->nonce_a;
    a.mac = agent_proof(key_, c->nonce_m, c->nonce_a, uuid_);
    c->chan->send(make_msg(MsgType::HelloAck, a));
}

void Agent::on_auth(Conn* c, const Msg& m) {
    AuthMsg a;
    if (!c->hello || !parse_msg(m, &a) ||
        !proof_equal(a.mac, master_proof(key_, c->nonce_m, c->nonce_a, c->master_instance))) {
        LOG_WARN("master at %s failed authentication; closing", c->peer.c_str());
        return close_conn(c);
    }
    c->authed = true;
    c->chan->enable_mac(session_key(key_, c->nonce_m, c->nonce_a));
    LOG_INFO("authenticated master (instance %016" PRIx64 ") at %s", c->master_instance, c->peer.c_str());
}

void Agent::on_attach(Conn* c, const Msg& m) {
    NodeAttachMsg na;
    NodeAttachAckMsg ack;
    if (!parse_msg(m, &na) || na.incarnation == 0 || na.master_instance != c->master_instance) {
        ack.status = EPROTO;
        ack.error = "malformed NODE_ATTACH";
        c->chan->send(make_msg(MsgType::NodeAttachAck, ack));
        return;
    }
    // A new attachment always starts from nothing: whatever an earlier
    // incarnation left behind is destroyed first, and reported.
    uint32_t images = 0, contexts = 0;
    Conn* old = master_;
    self_fence("superseded by a new attach", &images, &contexts);
    if (old && old != c) close_conn(old);

    master_ = c;
    machine_ = na.machine;
    inc_ = na.incarnation;
    master_instance_ = na.master_instance;
    lease_ms_ = na.lease_ms;
    renew_ms_ = na.renew_ms ? na.renew_ms : 1000;
    hb_seq_ = 0;
    hb_sent_.clear();
    expected_peers_.clear();
    node_->set_incarnation(inc_);
    node_->set_self(machine_);
    links_.set_self(machine_);
    (void)probe_machine(&res_, cfg_.nodes);
    node_->set_resources(res_);

    ack.stale_images_destroyed = images;
    ack.stale_contexts_destroyed = contexts;
    ack.resources = res_;
    ack.rdma_ip = rdma_ip_;
    ack.rdma_port = rdma_port_;
    ack.pool_bytes = pool_.bytes();
    ack.capacity_bytes = capacity_bytes_;
    c->chan->send(make_msg(MsgType::NodeAttachAck, ack, 0, inc_));
    LOG_INFO("attached to master %s as machine %u, incarnation %" PRIu64 " (lease %u ms)", na.master_hostname.c_str(),
             machine_, inc_, lease_ms_);

    // Provisional lease from the attach itself; renewed by heartbeats.
    arm_lease(now_ns() + uint64_t(lease_ms_) * kMs);
    send_heartbeat();
    hb_timer_ = reactor_.add_timer(uint64_t(renew_ms_) * kMs, [this] { send_heartbeat(); }, uint64_t(renew_ms_) * kMs);
    report_timer_ = reactor_.add_timer(500 * kMs, [this] { send_report(); }, 2000 * kMs);
    if (na.view_ms)
        view_timer_ = reactor_.add_timer(uint64_t(na.view_ms) * kMs, [this] { send_view_sample(); }, uint64_t(na.view_ms) * kMs);
}

void Agent::send_heartbeat() {
    if (inc_ == 0 || !master_ || partitioned_) return;
    HeartbeatMsg hb;
    hb.seq = ++hb_seq_;
    hb_sent_[hb.seq] = now_ns();
    reply(master_, MsgType::Heartbeat, hb);
}

// What this machine's kernel says about the CPUs offered to the cluster and
// about the bodies running on them, for the master's /proc view.
void Agent::send_view_sample() {
    if (inc_ == 0 || !master_ || partitioned_) return;
    ViewSampleMsg v;
    v.clk_tck = uint32_t(sysconf(_SC_CLK_TCK));
    std::set<int> offered;
    for (const auto& n : res_.nodes) offered.insert(n.cpus.begin(), n.cpus.end());
    std::string stat;
    if (read_file("/proc/stat", &stat).is_ok()) {
        for (const std::string& line : split(stat, '\n')) {
            if (line.compare(0, 3, "cpu") != 0 || line.size() < 4 || !isdigit(static_cast<unsigned char>(line[3]))) continue;
            std::vector<std::string> f = split(line, ' ');
            int cpu = atoi(f[0].c_str() + 3);
            if (!offered.count(cpu)) continue;
            v.cpus.push_back(cpu);
            // Kernels older than the last counters simply have fewer columns.
            for (size_t k = 1; k <= 10; ++k) v.cpu_times.push_back(k < f.size() ? strtoull(f[k].c_str(), nullptr, 10) : 0);
        }
    }
    for (const auto& [xid, pid] : bodies_->pids()) {
        std::string s = read_attr("/proc/" + std::to_string(pid) + "/stat");
        size_t paren = s.rfind(')'); // the command name may contain anything
        if (paren == std::string::npos || paren + 2 >= s.size()) continue; // gone since it was listed
        std::vector<std::string> f = split(s.substr(paren + 2), ' ');
        if (f.size() < 13) continue;
        v.bodies.push_back(xid);
        v.bodies.push_back(uint64_t(static_cast<unsigned char>(f[0][0])));
        v.bodies.push_back(strtoull(f[11].c_str(), nullptr, 10));
        v.bodies.push_back(strtoull(f[12].c_str(), nullptr, 10));
    }
    reply(master_, MsgType::ViewSample, v);
}

void Agent::send_report() {
    if (inc_ == 0 || !master_ || partitioned_) return;
    refresh_memory(&res_);
    ResourceReportMsg r;
    for (const auto& n : res_.nodes) r.node_mem_free.push_back(n.mem_free);
    r.node_resident = node_->resident_by_node();
    r.pool_slots_total = pool_.slots_total();
    r.pool_slots_used = pool_.slots_used();
    r.images = node_->image_count();
    r.contexts = node_->context_count();
    r.bodies = bodies_->count();
    r.runnable = runnable_now();
    r.metrics = metrics_;
    bodies_->merge_metrics(&r.metrics);
    if (PeerLink* pl = links_.get(kMasterMachine); pl && pl->link) {
        const auto& st = pl->link->stats();
        r.metrics.set("rdma_send_queue_avg_ns", st.msgs_tx ? st.send_wait_total_ns / st.msgs_tx : 0);
        r.metrics.set("rdma_send_queue_max_ns", st.send_wait_max_ns);
        r.metrics.set("rdma_send_waits_for_credits", st.credit_stalls);
        r.metrics.set("rdma_send_waits_for_buffers", st.buffer_stalls);
    }
    reply(master_, MsgType::ResourceReport, r);
}

void Agent::arm_lease(uint64_t expiry_ns) {
    if (expiry_ns <= lease_expiry_ns_) return;
    lease_expiry_ns_ = expiry_ns;
    reactor_.cancel_timer(lease_timer_);
    uint64_t now = now_ns();
    Incarnation inc = inc_;
    lease_timer_ = reactor_.add_timer(expiry_ns > now ? expiry_ns - now : 0, [this, inc] {
        if (inc_ != inc || inc_ == 0) return;
        if (now_ns() < lease_expiry_ns_) return; // renewed meanwhile
        // The master has not renewed our lease: it may already consider this
        // incarnation dead and have given its pages to others. Stop now.
        self_fence("lease expired without renewal");
    });
}

void Agent::self_fence(const std::string& why, uint32_t* images, uint32_t* contexts, bool keep_links) {
    uint32_t i = 0, c = 0;
    if (inc_ != 0) {
        LOG_WARN("ending incarnation %" PRIu64 ": %s", inc_, why.c_str());
        ++fences_;
        metrics_.add("self_fences");
    }
    if (bodies_ && bodies_->count()) {
        LOG_WARN("destroying %u exported process bod%s", bodies_->count(), bodies_->count() == 1 ? "y" : "ies");
        bodies_->kill_all();
    }
    if (node_) node_->reset(why.c_str(), &i, &c);
    if (images) *images = i;
    if (contexts) *contexts = c;
    if (inc_ == 0) return;
    if (i || c) LOG_WARN("destroyed %u image(s) and %u execution context(s)", i, c);
    last_fenced_inc_ = inc_;
    last_fenced_images_ = i;
    last_fenced_contexts_ = c;
    reactor_.cancel_timer(hb_timer_);
    reactor_.cancel_timer(lease_timer_);
    reactor_.cancel_timer(report_timer_);
    reactor_.cancel_timer(view_timer_);
    hb_timer_ = lease_timer_ = report_timer_ = view_timer_ = 0;
    lease_expiry_ns_ = 0;
    expected_peers_.clear();
    // (the partition test keeps the stale link for its probe)
    if (!partitioned_ && !keep_links) links_.drop_all();
    inc_ = 0;
    node_->set_incarnation(0);
}

// ============================ RDMA ============================

bool Agent::on_rdma_request(const RdmaEnv::Incoming& in, std::vector<uint8_t>* reply, RdmaEnv::LinkReady* ready) {
    LinkTicket t;
    if (inc_ == 0 || !check_link_ticket(key_, in.priv, 0, &t)) {
        LOG_WARN("rdma: connection request from %s rejected (%s)", in.peer_ip.c_str(),
                 inc_ == 0 ? "not attached" : "bad ticket");
        return false;
    }
    // The ticket must be addressed to exactly this incarnation, and come
    // from the master instance we are attached to or a peer it announced.
    bool ok = t.dst == machine_ && t.dst_inc == inc_;
    if (t.src == kMasterMachine) ok = ok && t.src_inc == master_instance_;
    else {
        auto it = expected_peers_.find(t.src);
        ok = ok && it != expected_peers_.end() && it->second == t.src_inc;
    }
    if (!ok) {
        LOG_WARN("rdma: connection request from %s rejected (machine %u incarnation %" PRIu64 " is not authorized for this incarnation)",
                 in.peer_ip.c_str(), t.src, uint64_t(t.src_inc));
        metrics_.add("rdma_links_rejected");
        return false;
    }
    *reply = make_link_ticket(key_, 1, machine_, inc_, t.src, t.src_inc);
    MachineId src = t.src;
    Incarnation src_inc = t.src_inc, my_inc = inc_;
    *ready = [this, src, src_inc, my_inc](Status st, std::unique_ptr<RdmaLink> link) {
        if (!st.is_ok() || inc_ != my_inc) return;
        LOG_INFO("rdma: link established with machine %u (%s)", src, link->peer_ip().c_str());
        links_.add(src, src_inc, my_inc, std::move(link), [src](Status st) {
            if (!st.is_ok()) LOG_WARN("rdma: link setup with machine %u failed: %s", src, st.str().c_str());
        });
    };
    return true;
}

void Agent::peer_connect(const PeerMsg& p) {
    Incarnation my_inc = inc_;
    MachineId peer = p.peer;
    Incarnation peer_inc = p.peer_inc;
    auto report = [this, peer, my_inc](Status st) {
        if (inc_ != my_inc || !master_) return;
        PeerMsg r;
        r.peer = peer;
        r.status = st.code();
        r.text = st.message();
        reply(master_, MsgType::PeerStatus, r);
    };
    std::vector<uint8_t> ticket = make_link_ticket(key_, 0, machine_, inc_, peer, peer_inc);
    rdma_.connect(p.rdma_ip, p.rdma_port, ticket, 8000,
                  [this, peer, peer_inc, my_inc, report](Status st, std::unique_ptr<RdmaLink> link, std::vector<uint8_t> rep) {
        if (inc_ != my_inc) return;
        if (!st.is_ok()) return report(st);
        LinkTicket t;
        if (!check_link_ticket(key_, rep, 1, &t) || t.src != peer || t.src_inc != peer_inc || t.dst != machine_ || t.dst_inc != my_inc)
            return report(Status::err(EACCES, "peer failed RDMA link authentication"));
        links_.add(peer, peer_inc, my_inc, std::move(link), [this, peer, report](Status st) {
            if (!st.is_ok()) return report(st);
            links_.verify(peer, report);
        });
    });
}

void Agent::on_ops_msg(MachineId from, Msg&& m) {
    if (from != kMasterMachine) return; // peers only exchange bulk data
    if (inc_ == 0 || m.inc != inc_) {
        metrics_.add("stale_messages_dropped");
        return;
    }
    if (m.type == MsgType::XBegin || m.type == MsgType::XSysRet || m.type == MsgType::XPage ||
        m.type == MsgType::XStop || m.type == MsgType::XKill || m.type == MsgType::XClock)
        return bodies_->handle(std::move(m));
    if (m.type == MsgType::LinkVerify) {
        Incarnation inc = inc_;
        links_.verify(kMasterMachine, [this, inc](Status st) {
            if (inc_ != inc) return;
            SimpleMsg ack;
            ack.status = st.code();
            ack.text = st.message();
            links_.send(kMasterMachine, make_msg(MsgType::LinkVerifyAck, ack, 0, inc_));
        });
        return;
    }
    node_->handle(std::move(m));
}

// ============================ fault injection ============================

void Agent::stale_access_probe() {
    // Runs at the end of a simulated partition: this incarnation has been
    // cut off, and the master has (or should have) fenced it. Try to use the
    // credentials we still hold.
    PeerLink* pl = links_.get(kMasterMachine);
    auto finish = [this] {
        partitioned_ = false;
        rdma_.test_ignore_events(false);
        self_fence("simulated partition ended");
        links_.drop_all();
        if (master_) close_conn(master_);
    };
    if (!pl || !pl->link) {
        LOG_WARN("STALE-ACCESS-PROBE read: no link object left (already torn down)");
        return finish();
    }
    pl->link->test_freeze(false);
    pl->link->test_kick();
    pl = links_.get(kMasterMachine);
    if (!pl || !pl->link || !pl->link->is_up()) {
        LOG_WARN("STALE-ACCESS-PROBE read: DENIED (queue pair already in error state)");
        return finish();
    }
    LOG_WARN("STALE-ACCESS-PROBE: attempting RDMA READ of master pool with rkey 0x%x from the fenced incarnation", pl->remote.pool_rkey);
    pl->link->read(pl->inbox + 48 * 1024, pl->inbox_mr->lkey, pl->remote.pool_base, pl->remote.pool_rkey, 4096,
                   [this, finish](Status st, uint64_t) {
        if (st.is_ok()) LOG_ERROR("STALE-ACCESS-PROBE read: SUCCEEDED - stale credentials still work (this is a bug)");
        else LOG_WARN("STALE-ACCESS-PROBE read: DENIED (%s)", st.str().c_str());
        reactor_.post(finish);
    });
}

void Agent::on_fault(Conn* c, const Msg& m) {
    FaultInjectMsg f;
    SimpleMsg ack;
    if (!parse_msg(m, &f)) return;
    LOG_WARN("fault injection: %s (%" PRIu64 ")", f.what.c_str(), f.arg);
    if (f.what == "pull-delay") {
        node_->set_test_pull_delay(f.arg);
        ack.text = format("page pulls delayed by %" PRIu64 " ms", f.arg);
    } else if (f.what == "exit") {
        ack.text = "agent exiting abruptly";
        reply(c, MsgType::FaultInjectAck, ack);
        reactor_.add_timer(50 * kMs, [] { _exit(9); });
        return;
    } else if (f.what == "drop-rdma") {
        links_.drop(kMasterMachine);
        ack.text = "RDMA link to master destroyed";
    } else if (f.what == "partition") {
        // Go silent on every channel without telling anyone, keep the stale
        // RDMA state, and after `arg` ms try to use it.
        ack.text = format("simulating a partition for %" PRIu64 " ms", f.arg);
        reply(c, MsgType::FaultInjectAck, ack);
        partitioned_ = true;
        rdma_.test_ignore_events(true);
        if (PeerLink* pl = links_.get(kMasterMachine)) pl->link->test_freeze(true);
        reactor_.add_timer((f.arg ? f.arg : 1) * kMs, [this] { stale_access_probe(); });
        return;
    } else {
        ack.status = EINVAL;
        ack.text = "unknown fault '" + f.what + "' (known: pull-delay <ms>, exit, drop-rdma, partition <ms>)";
    }
    reply(c, MsgType::FaultInjectAck, ack);
}

} // namespace dnuma
