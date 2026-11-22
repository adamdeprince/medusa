// SPDX-License-Identifier: GPL-2.0-only
// Logical processes, execution contexts and application calls.
//
// A logical process is identified by a ProcId assigned here. Natively it is a
// set of Linux processes: the *home image* on the master (the process the
// user launched, which owns the Linux-visible identity: PID, file
// descriptors, credentials, working directory) and at most one *shadow
// image* per slave, created on demand to host execution contexts there.
#include <algorithm>
#include <cinttypes>
#include <cmath>

#include <unistd.h>

#include "common/log.h"
#include "common/util.h"
#include "master/master.h"

namespace dnuma {

namespace {
constexpr uint64_t kMs = 1000000ull;
}

Process* Master::find_proc(ProcId id) {
    auto it = procs_.find(id);
    return it == procs_.end() || it->second.dying ? nullptr : &it->second;
}

ProcId Master::register_home(const RtHelloMsg& hello, int pid, uint32_t uid, uint32_t gid, uint64_t* va_base,
                             uint64_t* va_size, std::string* err) {
    if (shutting_down_) {
        *err = "master is shutting down";
        return 0;
    }
    // Trust what the kernel says about the process, not what it claims.
    std::string link = "/proc/" + std::to_string(pid) + "/exe";
    char buf[4096];
    ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
    if (n <= 0) {
        *err = "cannot resolve executable of pid " + std::to_string(pid);
        return 0;
    }
    Process p;
    p.exe.assign(buf, size_t(n));
    Status st = sha256_file(link, &p.exe_hash);
    if (!st.is_ok()) {
        *err = "cannot hash executable: " + st.str();
        return 0;
    }
    p.id = next_proc_++;
    p.home_pid = pid;
    p.name = p.exe.substr(p.exe.rfind('/') + 1);
    p.argv = hello.argv;
    p.uid = uid;
    p.gid = gid;
    p.canary = hello.canary;
    p.start_ns = now_ns();
    p.images[kMasterMachine] = Process::ImageState::Creating;
    *va_base = cfg_.va_base;
    *va_size = cfg_.va_size;
    coh_.create_space(p.id, page_of(cfg_.va_base), (cfg_.va_size - kReservedTailBytes) / kPageSize);
    ProcId id = p.id;
    procs_.emplace(id, std::move(p));
    metrics_.add("processes_started");
    return id;
}

void Master::destroy_process(ProcId id, const std::string& reason, bool forced) {
    auto it = procs_.find(id);
    if (it == procs_.end() || it->second.dying) return;
    Process& p = it->second;
    p.dying = true;
    if (forced) {
        LOG_WARN("process %" PRIu64 " (PID %d %s) TERMINATED: %s", id, p.home_pid, p.name.c_str(), reason.c_str());
        metrics_.add("forced_process_terminations");
        kill_log_.push_back(format("PID %d %s: %s", p.home_pid, p.name.c_str(), reason.c_str()));
        while (kill_log_.size() > 64) kill_log_.pop_front();
    } else {
        LOG_INFO("process %" PRIu64 " (PID %d %s) finished: %s", id, p.home_pid, p.name.c_str(), reason.c_str());
    }
    // Every machine that might hold an image or pages forgets the process;
    // the node kills its image with SIGKILL.
    std::set<MachineId> machines{kMasterMachine};
    for (auto& [m, st] : p.images) machines.insert(m);
    for (auto& [name, s] : slaves_)
        if (lifecycle_serving(s->state)) machines.insert(s->machine);
    MemoryMsg rel;
    rel.proc = id;
    for (MachineId m : machines) send_to_machine(m, make_msg(MsgType::MemoryRelease, rel));
    coh_.destroy_space(id);
    procs_.erase(id);
    metrics_.add("processes_ended");
    for (auto& [name, s] : slaves_)
        if (s->state == SlaveState::Draining) drain_step(s->machine);
}

// ============================ images ============================

void Master::on_image_ready(MachineId from, const Msg& m) {
    ImageMsg im;
    if (!parse_msg(m, &im)) return;
    Process* p = find_proc(im.proc);
    if (!p) return;
    if (from == kMasterMachine) {
        if (im.status != 0) return destroy_process(p->id, "home image setup failed: " + im.text, true);
        p->home_ready = true;
        p->images[kMasterMachine] = Process::ImageState::Ready;
        p->image_flags = im.flags;
        return;
    }
    auto waiters = std::move(p->image_waiters[from]);
    p->image_waiters.erase(from);
    Status st;
    if (im.status == 0) {
        p->images[from] = Process::ImageState::Ready;
    } else {
        p->images.erase(from);
        st = Status::err(im.status, "cannot create execution image on %s: %s", machine_name(from).c_str(), im.text.c_str());
        LOG_WARN("process %" PRIu64 ": %s", p->id, st.str().c_str());
    }
    ProcId id = p->id;
    for (auto& fn : waiters) {
        if (!find_proc(id)) break;
        fn(st);
    }
}

void Master::on_image_exited(MachineId from, const Msg& m) {
    ImageMsg im;
    if (!parse_msg(m, &im)) return;
    Process* p = find_proc(im.proc);
    if (!p) return;
    auto it = p->images.find(from);
    bool expected = it != p->images.end() && it->second == Process::ImageState::Destroying;
    if (it != p->images.end()) p->images.erase(it);
    if (from == kMasterMachine)
        return destroy_process(p->id, format("home image exited with status %d", im.status), false);
    if (expected) return drain_step(from);
    // A shadow image hosts threads of the process; losing it is like losing
    // threads to SIGKILL, so the whole logical process goes.
    destroy_process(p->id, format("its image on %s died unexpectedly (status %d)", machine_name(from).c_str(), im.status), true);
}

void Master::ensure_image(Process& p, MachineId m, std::function<void(Status)> done) {
    if (m == kMasterMachine) return done(p.home_ready ? Status::ok() : Status::err(EAGAIN, "home image is not ready"));
    auto it = p.images.find(m);
    if (it != p.images.end() && it->second == Process::ImageState::Ready) return done(Status::ok());
    if (it != p.images.end() && it->second == Process::ImageState::Destroying)
        return done(Status::err(EBUSY, "image on %s is being destroyed", machine_name(m).c_str()));
    p.image_waiters[m].push_back(std::move(done));
    if (it != p.images.end()) return; // creation already under way
    p.images[m] = Process::ImageState::Creating;
    ImageMsg im;
    im.proc = p.id;
    im.exe = p.exe;
    im.exe_hash.assign(p.exe_hash.begin(), p.exe_hash.end());
    im.argv = p.argv;
    im.va_base = cfg_.va_base;
    im.va_size = cfg_.va_size;
    im.canary = p.canary;
    im.uid = p.uid;
    im.gid = p.gid;
    send_to_machine(m, make_msg(MsgType::ImageCreate, im, next_req_++));
    ProcId id = p.id;
    reactor_.add_timer(cfg_.op_timeout_ms * kMs, [this, id, m] {
        Process* p = find_proc(id);
        if (!p) return;
        auto it = p->images.find(m);
        if (it == p->images.end() || it->second != Process::ImageState::Creating) return;
        p->images.erase(it);
        auto waiters = std::move(p->image_waiters[m]);
        p->image_waiters.erase(m);
        for (auto& fn : waiters) fn(Status::err(ETIMEDOUT, "image creation on %s timed out", machine_name(m).c_str()));
    });
}

// ============================ application calls ============================

void Master::app_reply(const AppCallMsg& call, int status, const std::string& error, std::vector<uint8_t> body) {
    AppCallMsg r;
    r.proc = call.proc;
    r.origin = call.origin;
    r.call = call.call;
    r.op = call.op;
    r.status = status;
    r.error = error;
    r.body = std::move(body);
    send_to_machine(call.origin, make_msg(MsgType::AppReply, r));
}

Status Master::resolve_node(int logical, bool for_placement, const NodeRecord** out) {
    *out = nullptr;
    if (logical < 0) return Status::ok();
    const NodeRecord* rec = topo_.by_logical(logical);
    if (!rec) return Status::err(EINVAL, "no such NUMA node %d", logical);
    if (!rec->present)
        return Status::err(ENODEV, "NUMA node %d belongs to %s, which is not attached", logical, machine_name(rec->machine).c_str());
    if (for_placement && rec->machine != kMasterMachine) {
        Slave* s = slave_by_machine(rec->machine);
        if (!s || !lifecycle_placeable(s->state))
            return Status::err(ENODEV, "NUMA node %d is on %s, which is %s and not accepting new work", logical,
                               machine_name(rec->machine).c_str(), s ? to_string(s->state) : "DISCONNECTED");
    }
    *out = rec;
    return Status::ok();
}

void Master::do_alloc(Process& p, const AppCallMsg& call) {
    AllocReq rq;
    if (!decode_body(call.body, &rq) || rq.bytes == 0) return app_reply(call, EINVAL, "bad allocation request", {});
    const NodeRecord* rec = nullptr;
    Status st = resolve_node(rq.logical_node, true, &rec);
    if (!st.is_ok()) return app_reply(call, st.code(), st.message(), {});
    if (rq.bytes > UINT64_MAX - kPageSize) return app_reply(call, ENOMEM, "allocation too large", {});
    uint64_t npages = (rq.bytes + kPageSize - 1) / kPageSize;
    PageNo first = 0;
    st = coh_.allocate(p.id, npages, rec ? rec->id : kNoNode, &first);
    if (!st.is_ok()) return app_reply(call, st.code(), st.message(), {});
    metrics_.add("allocations");
    metrics_.add("allocated_pages", npages);
    AddrMsg out;
    out.addr = addr_of(first);
    out.len = npages * kPageSize;
    bool populate = (rq.flags & 1) && rec;
    if (!populate) return app_reply(call, 0, "", encode_body(out));

    // Populate: back the allocation physically on the requested node now.
    MachineId m = rec->machine;
    if (free_pages(m) < npages) {
        coh_.free_region(p.id, first, [](Status) {});
        return app_reply(call, ENOMEM, format("NUMA node %d has no capacity for %s", rq.logical_node, human_bytes(rq.bytes).c_str()), {});
    }
    MemoryMsg mm;
    mm.proc = p.id;
    mm.first = first;
    mm.count = npages;
    mm.node = rec->id;
    uint64_t req = next_req_++;
    ProcId id = p.id;
    int logical = rq.logical_node;
    pending_mem_[req] = [this, call, id, first, npages, m, out, logical](const MemoryMsg& ack) {
        if (!find_proc(id)) return;
        coh_.mark_populated(id, first, ack.count, m);
        if (ack.status != 0 || ack.count != npages) {
            uint64_t got = ack.count;
            coh_.free_region(id, first, [](Status) {});
            return app_reply(call, ENOMEM, format("NUMA node %d could back only %" PRIu64 " of %" PRIu64 " pages (registered pool exhausted)", logical, got, npages), {});
        }
        app_reply(call, 0, "", encode_body(out));
    };
    send_to_machine(m, make_msg(MsgType::MemoryAllocate, mm, req));
}

void Master::do_thread_create(Process& p, const AppCallMsg& call) {
    ThreadCreateReq rq;
    if (!decode_body(call.body, &rq) || rq.entry == 0) return app_reply(call, EINVAL, "bad thread request", {});
    const NodeRecord* rec = nullptr;
    Status st = resolve_node(rq.logical_node, true, &rec);
    // Explicit placement is never silently replaced by something else.
    if (!st.is_ok()) return app_reply(call, st.code(), st.message(), {});
    if (!rec) {
        // No preference: least-loaded node that accepts new work.
        std::map<NodeId, uint64_t> load;
        for (auto& [id, pr] : procs_)
            for (auto& [cid, c] : pr.ctxs) ++load[c.node];
        for (const NodeRecord& r : topo_.all()) {
            if (!r.present) continue;
            if (r.machine != kMasterMachine) {
                Slave* s = slave_by_machine(r.machine);
                if (!s || !lifecycle_placeable(s->state)) continue;
            }
            if (!rec || load[r.id] < load[rec->id]) rec = &r;
        }
        if (!rec) return app_reply(call, ENODEV, "no NUMA node accepts new execution contexts", {});
    }
    uint64_t stack_bytes = rq.stack_size ? rq.stack_size : cfg_.default_stack_bytes;
    uint64_t stack_pages = (stack_bytes + kPageSize - 1) / kPageSize;
    PageNo stack_first = 0;
    st = coh_.allocate(p.id, stack_pages, rec->id, &stack_first);
    if (!st.is_ok()) return app_reply(call, st.code(), st.message(), {});

    CtxId cid = next_ctx_++;
    ExecCtx& c = p.ctxs[cid];
    c.id = cid;
    c.auto_place = rq.logical_node == -1;
    c.proc = p.id;
    c.machine = rec->machine;
    c.node = rec->id;
    c.entry = rq.entry;
    c.arg = rq.arg;
    c.stack_first = stack_first;
    c.stack_pages = stack_pages;
    c.info.ctx = cid;
    c.info.logical_node = rec->logical;
    c.creators.emplace_back(call.origin, call.call);
    metrics_.add("exec_contexts_created");

    ProcId id = p.id;
    ensure_image(p, rec->machine, [this, id, cid, call](Status st) {
        Process* p = find_proc(id);
        if (!p) return;
        auto it = p->ctxs.find(cid);
        if (it == p->ctxs.end()) return;
        if (!st.is_ok()) {
            coh_.free_region(id, it->second.stack_first, [](Status) {});
            p->ctxs.erase(it);
            return app_reply(call, st.code(), st.message(), {});
        }
        start_ctx(*p, it->second);
    });
}

void Master::start_ctx(Process& p, ExecCtx& c) {
    ExecMsg e;
    e.proc = p.id;
    e.ctx = c.id;
    e.entry = c.entry;
    e.arg = c.arg;
    e.stack_base = addr_of(c.stack_first);
    e.stack_size = c.stack_pages * kPageSize;
    e.node = c.node;
    send_to_machine(c.machine, make_msg(MsgType::ExecCreate, e));
    send_to_machine(c.machine, make_msg(MsgType::ExecStart, e));
}

void Master::page_handoff(ProcId proc, CtxId ctx, MachineId from, CtxId peer, MachineId to, uint64_t cost) {
    Process* p = find_proc(proc);
    if (!p) return;
    auto a = p->ctxs.find(ctx), b = p->ctxs.find(peer);
    if (a == p->ctxs.end() || b == p->ctxs.end()) return;
    ExecCtx* mover = &a->second;
    ExecCtx* partner = &b->second;
    if (mover->machine != from || partner->machine != to ||
        mover->state != ExecCtx::State::Running || partner->state != ExecCtx::State::Running) return;
    if (!mover->auto_place) std::swap(mover, partner);
    if (!mover->auto_place) return;

    // Require repeated handoffs. Discount older observations by half each
    // second so a previous sharing phase cannot pay for a migration today.
    uint64_t now = now_ns();
    if (mover->peer != partner->id || !mover->peer_sample_ns) {
        mover->peer = partner->id;
        mover->peer_sample_ns = now;
        mover->handoff_ns = double(cost);
        return;
    }
    mover->handoff_ns *= std::exp2(-double(now - mover->peer_sample_ns) / 1e9);
    mover->peer_sample_ns = now;
    mover->handoff_ns += double(cost);
    const Histogram* latency = metrics_.hist("thread_migration_ns");
    // Bootstrap from the measured lab migration latency until this master
    // has completed a migration of its own.
    uint64_t move_cost = latency && latency->count() ? latency->quantile(0.5) : 5 * kMs;
    if (mover->handoff_ns < double(move_cost) || now - mover->t_migrate < move_cost) return;
    const NodeRecord* dest = topo_.by_id(partner->node);
    if (!dest || !dest->present) return;
    if (dest->machine != kMasterMachine) {
        Slave* slave = slave_by_machine(dest->machine);
        if (!slave || !lifecycle_placeable(slave->state)) return;
    }
    size_t load = 0;
    for (const auto& [id, process] : procs_)
        for (const auto& [cid, c] : process.ctxs)
            if (c.state != ExecCtx::State::Exited &&
                (c.node == dest->id || (c.state == ExecCtx::State::Suspending && c.migrate_node == dest->id))) ++load;
    if (load >= dest->cpus.size()) return;
    if (request_ctx_migration(*p, *mover, dest->id).is_ok()) {
        metrics_.add("locality_migration_requests");
        mover->peer_sample_ns = partner->peer_sample_ns = 0;
    }
}

Status Master::request_ctx_migration(Process& p, ExecCtx& c, NodeId dest) {
    if (c.state != ExecCtx::State::Running)
        return Status::err(EBUSY, "execution context %" PRIu64 " is not running", c.id);
    if (c.node == dest) return Status::ok();
    c.migrate_node = dest;
    c.state = ExecCtx::State::Suspending;
    c.t_migrate = now_ns();
    metrics_.add("thread_migration_attempts");
    ExecMsg e;
    e.proc = p.id;
    e.ctx = c.id;
    send_to_machine(c.machine, make_msg(MsgType::ExecSuspend, e));
    return Status::ok();
}

void Master::resume_ctx(Process& p, ExecCtx& c) {
    ExecMsg e;
    e.proc = p.id;
    e.ctx = c.id;
    e.entry = c.entry;
    e.arg = c.arg;
    e.stack_base = addr_of(c.stack_first);
    e.stack_size = c.stack_pages * kPageSize;
    e.node = c.node;
    e.sp = c.saved_sp;
    send_to_machine(c.machine, make_msg(MsgType::ExecMigrate, e));
}

void Master::finish_ctx_exit(Process& p, CtxId id) {
    auto it = p.ctxs.find(id);
    if (it == p.ctxs.end()) return;
    coh_.free_region(p.id, it->second.stack_first, [](Status) {});
    p.ctxs.erase(it);
}

void Master::on_exec_event(MachineId from, const Msg& m) {
    ExecMsg e;
    if (!parse_msg(m, &e)) return;
    Process* p = find_proc(e.proc);
    if (!p) return;
    auto it = p->ctxs.find(e.ctx);
    if (it == p->ctxs.end()) return;
    ExecCtx& c = it->second;
    if (c.machine != from) return; // from a machine the context no longer lives on
    ProcId pid = p->id;

    if (m.type == MsgType::ExecStarted) {
        if (e.status != 0) {
            if (c.state == ExecCtx::State::Moving) {
                metrics_.add("thread_migration_failures");
                return destroy_process(pid, format("execution context %" PRIu64 " could not be resumed on %s", c.id, machine_name(from).c_str()), true);
            }
            auto creators = std::move(c.creators);
            coh_.free_region(pid, c.stack_first, [](Status) {});
            p->ctxs.erase(it);
            for (auto& [origin, callid] : creators) {
                AppCallMsg call;
                call.proc = pid;
                call.origin = origin;
                call.call = callid;
                call.op = AppOp::ThreadCreate;
                app_reply(call, e.status, format("execution context could not be started on %s", machine_name(from).c_str()), {});
            }
            return;
        }
        bool moved = c.state == ExecCtx::State::Moving;
        c.state = ExecCtx::State::Running;
        c.info.host = e.host;
        c.info.cpu = e.cpu;
        c.info.tid = e.tid;
        c.info.phys_node = e.phys_node;
        const NodeRecord* rec = topo_.by_id(c.node);
        c.info.logical_node = rec ? rec->logical : -1;
        if (moved) {
            ++c.info.migrations;
            metrics_.add("thread_migrations");
            metrics_.observe("thread_migration_ns", now_ns() - c.t_migrate);
            LOG_INFO("process %" PRIu64 ": context %" PRIu64 " migrated to %s (cpu %d)", pid, c.id, e.host.c_str(), e.cpu);
        }
        for (auto& [origin, callid] : c.creators) {
            AppCallMsg call;
            call.proc = pid;
            call.origin = origin;
            call.call = callid;
            call.op = AppOp::ThreadCreate;
            app_reply(call, 0, "", encode_body(c.info));
        }
        c.creators.clear();
    } else if (m.type == MsgType::ExecSuspended) {
        if (c.state != ExecCtx::State::Suspending) return;
        c.saved_sp = e.sp;
        // Choose where it resumes. If the requested destination is no longer
        // usable it resumes where it was: nothing is lost either way.
        const NodeRecord* rec = topo_.by_id(c.migrate_node);
        bool ok = rec && rec->present && (rec->machine == kMasterMachine || machine_usable(rec->machine));
        if (ok) {
            c.machine = rec->machine;
            c.node = rec->id;
        } else {
            metrics_.add("thread_migration_failures");
        }
        c.state = ExecCtx::State::Moving;
        CtxId cid = c.id;
        ensure_image(*p, c.machine, [this, pid, cid](Status st) {
            Process* p = find_proc(pid);
            if (!p) return;
            auto it = p->ctxs.find(cid);
            if (it == p->ctxs.end()) return;
            if (!st.is_ok()) {
                metrics_.add("thread_migration_failures");
                return destroy_process(pid, "execution context lost its destination image: " + st.message(), true);
            }
            resume_ctx(*p, it->second);
        });
    } else { // ExecExited
        c.state = ExecCtx::State::Exited;
        c.info.result = e.result;
        c.info.flags = e.flags;
        metrics_.add("exec_contexts_exited");
        if (!c.joiners.empty()) {
            auto joiners = std::move(c.joiners);
            ThreadInfo info = c.info;
            finish_ctx_exit(*p, c.id);
            for (auto& [origin, callid] : joiners) {
                AppCallMsg call;
                call.proc = pid;
                call.origin = origin;
                call.call = callid;
                call.op = AppOp::ThreadJoin;
                app_reply(call, 0, "", encode_body(info));
            }
        }
    }
    for (auto& [name, s] : slaves_)
        if (s->state == SlaveState::Draining) drain_step(s->machine);
}

void Master::on_syscall(MachineId from, const Msg& m) {
    SyscallMsg s;
    if (!parse_msg(m, &s) || !find_proc(s.proc)) return;
    if (m.type == MsgType::SyscallRequest) {
        // Executed by the home image, i.e. inside the logical process's
        // authoritative Linux context on the master.
        metrics_.add("forwarded_syscalls");
        send_to_machine(kMasterMachine, make_msg(MsgType::SyscallRequest, s));
    } else if (from == kMasterMachine) {
        send_to_machine(s.origin, make_msg(MsgType::SyscallComplete, s));
    }
}

void Master::do_migrate_pages(Process& p, const AppCallMsg& call) {
    AddrMsg rq;
    const NodeRecord* rec = nullptr;
    if (!decode_body(call.body, &rq) || rq.len == 0) return app_reply(call, EINVAL, "bad range", {});
    Status st = resolve_node(rq.logical_node, true, &rec);
    if (st.is_ok() && !rec) st = Status::err(EINVAL, "a destination NUMA node is required");
    if (!st.is_ok()) return app_reply(call, st.code(), st.message(), {});
    struct Batch {
        std::deque<PageNo> work;
        uint32_t inflight = 0;
        uint64_t moved = 0, failed = 0;
        Status first_err;
        bool pumping = false, finished = false;
        std::function<void()> pump;
    };
    auto b = std::make_shared<Batch>();
    for (PageNo pg = page_of(rq.addr); pg <= page_of(rq.addr + rq.len - 1); ++pg) {
        const PageEntry* e = coh_.lookup(p.id, pg);
        if (e && (e->state == PageState::Exclusive || e->state == PageState::Shared)) b->work.push_back(pg);
    }
    ProcId id = p.id;
    MachineId dest = rec->machine;
    uint32_t window = cfg_.drain_window;
    b->pump = [this, b, id, dest, call, window] {
        if (b->pumping) return;
        b->pumping = true;
        while (b->inflight < window && !b->work.empty()) {
            PageNo pg = b->work.front();
            b->work.pop_front();
            if (free_pages(dest) == 0 && !(coh_.lookup(id, pg) && coh_.lookup(id, pg)->owner == dest)) {
                ++b->failed;
                if (b->first_err.is_ok()) b->first_err = Status::err(ENOMEM, "destination machine has no capacity");
                continue;
            }
            ++b->inflight;
            coh_.migrate(id, pg, dest, [b](Status st) {
                --b->inflight;
                if (st.is_ok()) ++b->moved;
                else {
                    ++b->failed;
                    if (b->first_err.is_ok()) b->first_err = st;
                }
                if (b->pump && !b->finished) b->pump();
            });
        }
        b->pumping = false;
        if (b->work.empty() && b->inflight == 0 && !b->finished) {
            b->finished = true; // reply exactly once
            AddrMsg out;
            out.a0 = b->moved;
            out.a1 = b->failed;
            if (find_proc(id)) app_reply(call, b->failed ? b->first_err.code() : 0, b->failed ? b->first_err.message() : "", encode_body(out));
            // Break the batch <-> closure reference cycle, but not while the
            // closure is still executing.
            reactor_.post([b] { b->pump = nullptr; });
        }
    };
    auto pump = b->pump;
    pump();
}

void Master::on_app_call(MachineId from, const Msg& m) {
    AppCallMsg call;
    if (!parse_msg(m, &call)) return;
    call.origin = from; // the transport, not the payload, says where it came from
    Process* p = find_proc(call.proc);
    if (!p) return;
    switch (call.op) {
    case AppOp::Alloc:
        return do_alloc(*p, call);
    case AppOp::Free: {
        AddrMsg rq;
        if (!decode_body(call.body, &rq)) return app_reply(call, EINVAL, "bad request", {});
        coh_.free_region(p->id, page_of(rq.addr), [this, call](Status st) { app_reply(call, st.code(), st.message(), {}); });
        return;
    }
    case AppOp::ThreadCreate:
        return do_thread_create(*p, call);
    case AppOp::ThreadJoin: {
        AddrMsg rq;
        if (!decode_body(call.body, &rq)) return app_reply(call, EINVAL, "bad request", {});
        auto it = p->ctxs.find(rq.a0);
        if (it == p->ctxs.end()) return app_reply(call, ESRCH, "no such execution context", {});
        if (it->second.state == ExecCtx::State::Exited) {
            ThreadInfo info = it->second.info;
            finish_ctx_exit(*p, rq.a0);
            return app_reply(call, 0, "", encode_body(info));
        }
        it->second.joiners.emplace_back(call.origin, call.call);
        return;
    }
    case AppOp::ThreadMigrate: {
        AddrMsg rq;
        const NodeRecord* rec = nullptr;
        if (!decode_body(call.body, &rq)) return app_reply(call, EINVAL, "bad request", {});
        auto it = p->ctxs.find(rq.a0);
        if (it == p->ctxs.end()) return app_reply(call, ESRCH, "no such execution context", {});
        Status st = resolve_node(rq.logical_node, true, &rec);
        if (st.is_ok() && !rec) st = Status::err(EINVAL, "a destination NUMA node is required");
        if (st.is_ok()) st = request_ctx_migration(*p, it->second, rec->id);
        if (st.is_ok()) it->second.auto_place = false;
        return app_reply(call, st.code(), st.message(), {});
    }
    case AppOp::Topology: {
        TopologyReply rep;
        for (const NodeRecord& r : topo_.all()) {
            TopoNode n;
            n.logical = r.logical;
            n.node = r.id;
            n.machine = r.machine;
            n.host = machine_host(r.machine); // the physical host, not the administrative name
            n.phys_index = r.phys_index;
            n.ncpus = uint32_t(r.cpus.size());
            n.mem_total = r.mem_total;
            n.mem_free = r.mem_free;
            n.is_master = r.machine == kMasterMachine;
            Slave* s = n.is_master ? nullptr : slave_by_machine(r.machine);
            n.state = n.is_master ? SlaveState::Active : (s && r.present ? s->state : SlaveState::Disconnected);
            rep.nodes.push_back(std::move(n));
        }
        return app_reply(call, 0, "", encode_body(rep));
    }
    case AppOp::PageInfo: {
        AddrMsg rq;
        if (!decode_body(call.body, &rq)) return app_reply(call, EINVAL, "bad request", {});
        NodeId home = kNoNode;
        const PageEntry* e = coh_.lookup(p->id, page_of(rq.addr), &home);
        if (!e) return app_reply(call, EFAULT, "address is not part of a distributed allocation", {});
        PageInfoReply rep;
        rep.state = e->state;
        rep.owner = e->owner;
        rep.owner_host = e->owner == kNoMachine ? "" : machine_host(e->owner);
        rep.gen = e->gen;
        rep.readers = e->readers;
        const NodeRecord* rec = topo_.by_id(home);
        rep.home_logical = rec ? rec->logical : -1;
        return app_reply(call, 0, "", encode_body(rep));
    }
    case AppOp::MigratePages:
        return do_migrate_pages(*p, call);
    }
    app_reply(call, ENOSYS, "unknown application call", {});
}

} // namespace dnuma
