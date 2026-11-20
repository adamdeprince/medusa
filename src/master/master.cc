// SPDX-License-Identifier: GPL-2.0-only
#include "master/master.h"

#include <cinttypes>

#include <sys/epoll.h>
#include <sys/stat.h>
#include <time.h>

#include "common/log.h"
#include "common/util.h"

namespace dnuma {

void CtlSession::out(const std::string& text) {
    if (finished || !chan || !chan->is_open()) return;
    CtlOutputMsg o;
    o.text = text;
    chan->send(make_msg(MsgType::CtlOutput, o));
}

void CtlSession::done(int code) {
    if (finished) return;
    finished = true;
    if (!chan || !chan->is_open()) return;
    CtlDoneMsg d;
    d.code = code;
    chan->send(make_msg(MsgType::CtlDone, d));
}

Master::Master(Reactor& reactor, MasterConfig cfg)
    : reactor_(reactor), cfg_(std::move(cfg)), rdma_(reactor), links_(kMasterMachine, pool_, node_metrics_),
      coh_(*this, metrics_) {}

Master::~Master() { shutdown(); }

namespace {
Status ensure_parent_dir(const std::string& path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return Status::ok();
    std::string dir = path.substr(0, slash);
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return Status::sys("mkdir %s", dir.c_str());
    return Status::ok();
}
} // namespace

Status Master::init() {
    DNUMA_TRY(load_cluster_key(cfg_.key_file, &key_));
    instance_ = random_u64() | 1;
    // Incarnations never repeat across master restarts: the high bits are
    // the wall-clock start time, the low bits a counter.
    next_inc_ = uint64_t(time(nullptr)) << 20;

    DNUMA_TRY(probe_machine(&res_));
    local_capacity_pages_ = (cfg_.capacity_bytes ? cfg_.capacity_bytes : res_.mem_available / 2) / kPageSize;

    std::vector<int> phys;
    for (const auto& n : res_.nodes) phys.push_back(n.phys_index);
    DNUMA_TRY(pool_.init(cfg_.pool_bytes, phys));
    DNUMA_TRY(rdma_.init());

    NodeConfig nc;
    nc.self = kMasterMachine;
    nc.is_master = true;
    nc.hostname = res_.hostname;
    NodeCore::Hooks hooks;
    hooks.to_coordinator = [this](Msg m) {
        // Deferred so node and coordinator never re-enter each other.
        reactor_.post([this, m = std::move(m)]() mutable { on_node_msg(kMasterMachine, std::move(m)); });
    };
    hooks.register_home = [this](const RtHelloMsg& hello, int pid, uint32_t uid, uint32_t gid, uint64_t* va_base,
                                 uint64_t* va_size, std::string* err) {
        return register_home(hello, pid, uid, gid, va_base, va_size, err);
    };
    node_ = std::make_unique<NodeCore>(reactor_, nc, pool_, links_, node_metrics_, std::move(hooks));
    node_->set_resources(res_);
    node_->set_node_map(topo_.attach(kMasterMachine, "master", res_));

    links_.set_handlers(
        [this](MachineId from, Msg&& m) { on_node_msg(from, std::move(m)); },
        [this](MachineId peer, Incarnation inc, const Status& st) {
            Slave* s = slave_by_machine(peer);
            if (!s || s->inc != inc) return;
            // During the final detach of a drain the agent closes the link itself.
            if (s->state == SlaveState::Draining && s->drain && s->drain->phase == DrainOp::Phase::Detach) return;
            slave_failed(*s, "RDMA link lost: " + st.str());
        });

    DNUMA_TRY(ensure_parent_dir(cfg_.ctl_sock));
    DNUMA_TRY(ensure_parent_dir(cfg_.app_sock));
    DNUMA_TRY(unix_listen(cfg_.ctl_sock, 0660, &ctl_listen_));
    // Applications of any user may register; they are identified with
    // SO_PEERCRED and can only ever act on their own logical process.
    DNUMA_TRY(unix_listen(cfg_.app_sock, 0666, &app_listen_));
    DNUMA_TRY(reactor_.add_fd(ctl_listen_.get(), EPOLLIN, [this](uint32_t) { on_ctl_accept(); }));
    DNUMA_TRY(reactor_.add_fd(app_listen_.get(), EPOLLIN, [this](uint32_t) {
        for (;;) {
            Fd fd = accept_one(app_listen_.get(), nullptr);
            if (!fd.valid()) break;
            node_->adopt_connection(std::move(fd));
        }
    }));

    export_init();
    reactor_.add_timer(1000000000ull, [this] {
        refresh_memory(&res_);
        std::vector<uint64_t> free;
        for (const auto& n : res_.nodes) free.push_back(n.mem_free);
        topo_.update_free(kMasterMachine, free);
    }, 1000000000ull);

    UffdCaps uc = uffd_caps_from_features(res_.uffd_features, res_.uffd_dev);
    LOG_INFO("dnuma-master %s (kernel UAPI %s) on %s, kernel %s: %d sockets, %d CPUs, %zu NUMA nodes, %s RAM",
             DNUMA_VERSION, DNUMA_UAPI_VERSION, res_.hostname.c_str(), res_.kernel.c_str(), res_.sockets,
             res_.cpus_online, res_.nodes.size(), human_bytes(res_.mem_total).c_str());
    LOG_INFO("userfaultfd: %s", uc.summary().c_str());
    LOG_INFO("page pool %s, local capacity %s, instance %016" PRIx64, human_bytes(pool_.bytes()).c_str(),
             human_bytes(local_capacity_pages_ * kPageSize).c_str(), instance_);
    return Status::ok();
}

void Master::shutdown() {
    if (shutting_down_) return;
    shutting_down_ = true;
    // The master is the only authority: without it no distributed process
    // can make progress, so they are terminated rather than left hanging.
    exports_.reset(); // anything still exported at this point cannot be recovered
    std::vector<ProcId> ids;
    for (auto& [id, p] : procs_) ids.push_back(id);
    for (ProcId id : ids) destroy_process(id, "master shutting down", true);
    for (auto& [name, s] : slaves_) {
        if (s->mgmt && s->mgmt->is_open() && lifecycle_serving(s->state)) {
            SimpleMsg f;
            f.text = "master shutting down";
            s->mgmt->send(make_msg(MsgType::NodeFence, f, 0, s->inc));
        }
    }
    links_.drop_all();
    node_.reset();
    if (!cfg_.ctl_sock.empty()) ::unlink(cfg_.ctl_sock.c_str());
    if (!cfg_.app_sock.empty()) ::unlink(cfg_.app_sock.c_str());
}

Slave* Master::slave_by_machine(MachineId m) {
    for (auto& [name, s] : slaves_)
        if (s->machine == m && s->state != SlaveState::Disconnected) return s.get();
    return nullptr;
}

Slave* Master::slave_by_name(const std::string& name) {
    auto it = slaves_.find(name);
    return it == slaves_.end() ? nullptr : it->second.get();
}

std::string Master::machine_name(MachineId m) {
    if (m == kMasterMachine) return res_.hostname;
    for (auto& [name, s] : slaves_)
        if (s->machine == m) return name;
    return "machine-" + std::to_string(m);
}

std::string Master::machine_host(MachineId m) {
    if (m == kMasterMachine) return res_.hostname;
    for (auto& [name, s] : slaves_)
        if (s->machine == m && !s->hostname.empty()) return s->hostname;
    return machine_name(m);
}

void Master::transition(Slave& s, LifecycleEvent ev, const std::string& why) {
    SlaveState next;
    if (!lifecycle_next(s.state, ev, s.fence_pending, &next)) {
        LOG_ERROR("slave %s: illegal lifecycle event %s in state %s (%s)", s.name.c_str(), to_string(ev),
                  to_string(s.state), why.c_str());
        return;
    }
    std::string line = format("%s -> %s (%s%s%s)", to_string(s.state), to_string(next), to_string(ev),
                              why.empty() ? "" : ": ", why.c_str());
    LOG_INFO("slave %s [incarnation %" PRIu64 "]: %s", s.name.c_str(), s.inc, line.c_str());
    s.history.push_back(line);
    while (s.history.size() > 32) s.history.pop_front();
    s.state = next;
    if (!lifecycle_serving(next)) view_slave_gone(s);
    metrics_.add("slave_lifecycle_transitions");
}

uint64_t Master::capacity_pages(MachineId m) {
    if (m == kMasterMachine) return local_capacity_pages_;
    Slave* s = slave_by_machine(m);
    return s ? s->capacity_pages : 0;
}

uint64_t Master::free_pages(MachineId m) {
    uint64_t cap = capacity_pages(m);
    uint64_t used = coh_.usage(m).resident + coh_.inflight_to(m);
    return used >= cap ? 0 : cap - used;
}

// ---------------- CoherenceIo ----------------

void Master::send_page(MachineId to, MsgType type, const PageMsg& m) { send_to_machine(to, make_msg(type, m)); }

Incarnation Master::incarnation_of(MachineId m) {
    if (m == kMasterMachine) return instance_;
    Slave* s = slave_by_machine(m);
    return s ? s->inc : 0;
}

bool Master::machine_usable(MachineId m) {
    if (m == kMasterMachine) return true;
    Slave* s = slave_by_machine(m);
    return s && lifecycle_serving(s->state);
}

void Master::coherence_failure(ProcId proc, const std::string& why) {
    // Called from inside the coherence engine: act after it has unwound.
    reactor_.post([this, proc, why] { destroy_process(proc, "coherence failure: " + why, true); });
}

// ---------------- routing ----------------

void Master::send_to_machine(MachineId m, Msg msg) {
    if (m == kMasterMachine) {
        reactor_.post([this, msg = std::move(msg)]() mutable {
            if (node_) node_->handle(std::move(msg));
        });
        return;
    }
    Slave* s = slave_by_machine(m);
    if (!s || !(lifecycle_serving(s->state) || s->state == SlaveState::Connecting)) return;
    msg.inc = s->inc; // every operation names the incarnation it is meant for
    links_.send(m, std::move(msg));
}

void Master::on_node_msg(MachineId from, Msg&& m) {
    if (shutting_down_) return;
    if (from != kMasterMachine) {
        Slave* s = slave_by_machine(from);
        if (!s || m.inc != s->inc || !(lifecycle_serving(s->state) || s->state == SlaveState::Connecting)) {
            metrics_.add("stale_messages_dropped");
            LOG_DEBUG("dropping %s from machine %u (incarnation %" PRIu64 "): not current", to_string(m.type),
                      from, m.inc);
            return;
        }
    }
    switch (m.type) {
    case MsgType::PageRequest:
    case MsgType::PageRevoked:
    case MsgType::PageTransferComplete: {
        PageMsg pm;
        if (!parse_msg(m, &pm)) break;
        if (m.type == MsgType::PageRequest) coh_.on_request(from, pm);
        else if (m.type == MsgType::PageRevoked) coh_.on_revoked(from, pm);
        else coh_.on_complete(from, pm);
        break;
    }
    case MsgType::ImageReady:
        on_image_ready(from, m);
        break;
    case MsgType::ImageExited:
        on_image_exited(from, m);
        break;
    case MsgType::ExecStarted:
    case MsgType::ExecSuspended:
    case MsgType::ExecExited:
        on_exec_event(from, m);
        break;
    case MsgType::SyscallRequest:
    case MsgType::SyscallComplete:
        on_syscall(from, m);
        break;
    case MsgType::AppCall:
        on_app_call(from, m);
        break;
    case MsgType::MemoryAllocateAck: {
        MemoryMsg mm;
        auto it = pending_mem_.find(m.req_id);
        if (!parse_msg(m, &mm) || it == pending_mem_.end()) break;
        auto cb = std::move(it->second);
        pending_mem_.erase(it);
        cb(mm);
        break;
    }
    case MsgType::XReady:
    case MsgType::XSyscall:
    case MsgType::XPageReq:
    case MsgType::XHome:
    case MsgType::XPagePush:
    case MsgType::XHomeDone:
    case MsgType::XGone:
    case MsgType::XClockReq:
        if (exports_) exports_->on_node_msg(from, std::move(m));
        break;
    case MsgType::LinkVerifyAck: {
        Slave* s = slave_by_machine(from);
        SimpleMsg ack;
        if (!s || !parse_msg(m, &ack)) break;
        on_link_verify_ack(*s, ack);
        break;
    }
    default:
        LOG_WARN("unexpected %s from machine %u", to_string(m.type), from);
    }
}

} // namespace dnuma
