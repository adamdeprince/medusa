// SPDX-License-Identifier: GPL-2.0-only
#include "node/node_core.h"
#include "node/native_pager.h"

#include <cinttypes>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <limits>
#include <spawn.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/log.h"
#include "common/sha256.h"
#include "common/util.h"

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

extern char** environ;

namespace dnuma {

namespace {
bool at_least(Access have, Access need) { return static_cast<int>(have) >= static_cast<int>(need); }
} // namespace

NodeCore::NodeCore(Reactor& reactor, NodeConfig cfg, PagePool& pool, LinkTable& links, Metrics& metrics,
                   Hooks hooks)
    : reactor_(reactor), cfg_(std::move(cfg)), pool_(pool), links_(links), metrics_(metrics),
      hooks_(std::move(hooks)), zero_page_(kPageSize, 0) {}

NodeCore::~NodeCore() {
    uint32_t a, b;
    reset("shutdown", &a, &b);
}

void NodeCore::set_node_map(const std::vector<NodeAssignEntry>& nodes) {
    node_phys_.clear();
    for (const auto& n : nodes) node_phys_[n.node] = n.phys_index;
}

int NodeCore::phys_of(NodeId node) const {
    auto it = node_phys_.find(node);
    return it == node_phys_.end() ? -1 : it->second;
}

std::vector<int32_t> NodeCore::cpus_of(int phys) const {
    for (const auto& n : res_.nodes)
        if (n.phys_index == phys) return n.cpus;
    return {};
}

NodeCore::Proc* NodeCore::find_proc(ProcId id) {
    auto it = procs_.find(id);
    return it == procs_.end() ? nullptr : &it->second;
}

NodeCore::Proc& NodeCore::get_proc(ProcId id) {
    Proc& p = procs_[id];
    p.id = id;
    return p;
}

uint32_t NodeCore::image_count() const {
    uint32_t n = 0;
    for (const auto& [id, p] : procs_) n += p.image ? 1 : 0;
    return n;
}

uint32_t NodeCore::context_count() const {
    uint32_t n = 0;
    for (const auto& [id, p] : procs_)
        if (p.image) n += uint32_t(p.image->ctx_phys.size());
    return n;
}

std::vector<uint64_t> NodeCore::resident_by_node() const {
    std::vector<uint64_t> out;
    for (const auto& n : res_.nodes) {
        auto it = resident_.find(n.phys_index);
        out.push_back(it == resident_.end() ? 0 : it->second);
    }
    return out;
}

void NodeCore::account(LocalPage& lp, int phys, bool add) {
    if (add) {
        lp.phys_node = int16_t(phys);
        ++resident_[phys];
    } else {
        auto it = resident_.find(lp.phys_node);
        if (it != resident_.end() && it->second > 0) --it->second;
    }
}

void NodeCore::free_slot(int32_t slot) {
    if (slot >= 0) pool_.free(uint32_t(slot));
}

void NodeCore::maybe_erase(Proc& p, PageNo page) {
    auto it = p.pages.find(page);
    if (it == p.pages.end()) return;
    const LocalPage& lp = it->second;
    if (lp.right == Access::None && lp.where == Where::Absent && lp.requested == Access::None &&
        lp.export_slot < 0 && !lp.zapping)
        p.pages.erase(it);
}

// ============================ images ============================

void NodeCore::adopt_connection(Fd fd) {
    auto chan = std::make_unique<StreamChannel>(reactor_, std::move(fd), true);
    StreamChannel* raw = chan.get();
    greeting_[raw] = std::move(chan);
    auto bound = std::make_shared<ProcId>(0);
    raw->start(
        [this, raw, bound](Msg&& m) {
            if (*bound == 0) {
                auto it = greeting_.find(raw);
                if (it == greeting_.end()) return;
                if (m.type != MsgType::RtHello) {
                    LOG_WARN("runtime connection sent %s before RT_HELLO; closing", to_string(m.type));
                    close_fds(m.fds);
                    std::unique_ptr<StreamChannel> dead = std::move(it->second);
                    greeting_.erase(it);
                    reactor_.post([d = std::shared_ptr<StreamChannel>(std::move(dead))] {});
                    return;
                }
                std::unique_ptr<StreamChannel> ch = std::move(it->second);
                greeting_.erase(it);
                RtHelloMsg hello;
                ProcId proc = 0;
                if (parse_msg(m, &hello)) {
                    // on_rt_hello takes ownership of the channel on success.
                    on_rt_hello(ch, std::move(m), true);
                    for (auto& [id, p] : procs_)
                        if (p.image && p.image->chan.get() == raw) proc = id;
                }
                if (proc == 0 && ch) reactor_.post([d = std::shared_ptr<StreamChannel>(std::move(ch))] {});
                *bound = proc;
                return;
            }
            Proc* p = find_proc(*bound);
            if (!p || !p->image || p->image->chan.get() != raw) {
                close_fds(m.fds);
                return;
            }
            on_image_msg(p->image.get(), *bound, std::move(m));
        },
        [this, raw, bound](const Status&) {
            if (*bound == 0) {
                auto it = greeting_.find(raw);
                if (it == greeting_.end()) return;
                std::unique_ptr<StreamChannel> dead = std::move(it->second);
                greeting_.erase(it);
                reactor_.post([d = std::shared_ptr<StreamChannel>(std::move(dead))] {});
                return;
            }
            on_image_closed(*bound, raw);
        });
}

void NodeCore::on_rt_hello(std::unique_ptr<StreamChannel>& chan, Msg&& m, bool from_listener) {
    RtHelloMsg hello;
    if (!parse_msg(m, &hello)) return;
    RtWelcomeMsg w;
    w.machine = cfg_.self;
    w.hostname = cfg_.hostname;
    if (!from_listener || hello.role != ImageRole::Home || !cfg_.is_master || !hooks_.register_home) {
        w.status = EPERM;
        w.text = "only home images may register on the master application socket";
        chan->send(make_msg(MsgType::RtWelcome, w));
        return;
    }
    ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(chan->fd(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        w.status = errno;
        w.text = "cannot identify peer process";
        chan->send(make_msg(MsgType::RtWelcome, w));
        return;
    }
    std::string err;
    uint64_t va_base = 0, va_size = 0;
    ProcId proc = hooks_.register_home(hello, cred.pid, cred.uid, cred.gid, &va_base, &va_size, &err);
    if (proc == 0) {
        w.status = EAGAIN;
        w.text = err;
        chan->send(make_msg(MsgType::RtWelcome, w));
        return;
    }
    Proc& p = get_proc(proc);
    p.va_base = va_base;
    p.va_size = va_size;
    p.canary = hello.canary;
    p.uid = cred.uid;
    p.gid = cred.gid;
    p.image = std::make_unique<Image>();
    Image& img = *p.image;
    img.proc = proc;
    img.role = ImageRole::Home;
    img.pid = cred.pid;
    img.chan = std::move(chan);

    w.proc = proc;
    w.va_base = va_base;
    w.va_size = va_size;
    w.canary = hello.canary;
    w.uid = cred.uid;
    w.gid = cred.gid;
    Msg out = make_msg(MsgType::RtWelcome, w);
    // Delegate full (kernel-mode capable) userfaultfd creation to the image
    // where the kernel offers /dev/userfaultfd.
    int dev = ::open("/dev/userfaultfd", O_RDWR | O_CLOEXEC);
    if (dev >= 0) out.fds.push_back(dev);
    img.chan->send(std::move(out));
    LOG_INFO("proc %" PRIu64 ": home image pid %d registered", proc, img.pid);
}

void NodeCore::fail_image_create(ProcId proc, const std::string& why) {
    Proc* p = find_proc(proc);
    uint64_t req = (p && p->image) ? p->image->create_req : 0;
    LOG_ERROR("proc %" PRIu64 ": image creation failed: %s", proc, why.c_str());
    if (p && p->image) {
        reactor_.cancel_timer(p->image->start_timer);
        kill_image(*p->image);
    }
    ImageMsg r;
    r.proc = proc;
    r.status = EIO;
    r.text = why;
    Msg m = make_msg(MsgType::ImageReady, r, req, inc_);
    hooks_.to_coordinator(std::move(m));
}

void NodeCore::spawn_shadow(const ImageMsg& im, uint64_t req) {
    auto reply_fail = [&](const std::string& why) {
        LOG_ERROR("proc %" PRIu64 ": cannot create image: %s", im.proc, why.c_str());
        ImageMsg r;
        r.proc = im.proc;
        r.status = EIO;
        r.text = why;
        hooks_.to_coordinator(make_msg(MsgType::ImageReady, r, req, inc_));
    };
    Proc* existing = find_proc(im.proc);
    if (existing && existing->image) return reply_fail("an image for this process already exists");

    // The image must be the very same program as the home image: entry
    // points are raw code addresses.
    Digest d;
    Status st = sha256_file(im.exe, &d);
    if (!st.is_ok()) return reply_fail("cannot read executable: " + st.str());
    if (im.exe_hash.size() != d.size() || memcmp(im.exe_hash.data(), d.data(), d.size()) != 0)
        return reply_fail("executable " + im.exe + " differs from the master's copy (sha256 mismatch)");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        return reply_fail(std::string("socketpair: ") + strerror(errno));
    Fd parent_end(sv[0]), child_end(sv[1]);

    const int kChildFd = 3;
    std::vector<std::string> env_store;
    for (char** e = environ; e && *e; ++e)
        if (strncmp(*e, "DNUMA_", 6) != 0) env_store.emplace_back(*e);
    env_store.push_back("DNUMA_SHADOW_FD=" + std::to_string(kChildFd));
    env_store.push_back("DNUMA_PARENT_PID=" + std::to_string(getpid()));
    std::vector<char*> envp, argv;
    for (auto& s : env_store) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = im.argv;
    if (args.empty()) args.push_back(im.exe);
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, child_end.get(), kChildFd);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, im.exe.c_str(), &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return reply_fail(std::string("posix_spawn: ") + strerror(rc));
    child_end.reset();

    Proc& p = get_proc(im.proc);
    p.va_base = im.va_base;
    p.va_size = im.va_size;
    p.canary = im.canary;
    p.uid = im.uid;
    p.gid = im.gid;
    p.image = std::make_unique<Image>();
    Image& img = *p.image;
    img.proc = im.proc;
    img.role = ImageRole::Shadow;
    img.pid = pid;
    img.create_req = req;
    img.pidfd.reset(int(syscall(SYS_pidfd_open, pid, 0)));
    img.chan = std::make_unique<StreamChannel>(reactor_, std::move(parent_end), true);
    ProcId proc = im.proc;
    StreamChannel* raw = img.chan.get();
    img.chan->start(
        [this, proc, raw](Msg&& m) {
            Proc* p = find_proc(proc);
            if (!p || !p->image || p->image->chan.get() != raw) {
                close_fds(m.fds);
                return;
            }
            on_image_msg(p->image.get(), proc, std::move(m));
        },
        [this, proc, raw](const Status&) { on_image_closed(proc, raw); });
    if (img.pidfd.valid())
        (void)reactor_.add_fd(img.pidfd.get(), EPOLLIN, [this, proc](uint32_t) { on_image_exit(proc); });
    img.start_timer = reactor_.add_timer(cfg_.image_start_timeout_ms * 1000000ull, [this, proc] {
        Proc* p = find_proc(proc);
        if (p && p->image && !p->image->ready) fail_image_create(proc, "image did not become ready in time");
    });
    LOG_INFO("proc %" PRIu64 ": spawned shadow image pid %d (%s)", proc, pid, im.exe.c_str());
}

void NodeCore::on_rt_ready(Proc& p, Msg&& m) {
    Image& img = *p.image;
    RtReadyMsg r;
    if (!parse_msg(m, &r) || (r.status == 0 && m.fds.size() != 1)) {
        close_fds(m.fds);
        return fail_image_create(p.id, "malformed RT_READY");
    }
    if (r.status != 0) {
        close_fds(m.fds);
        return fail_image_create(p.id, "runtime setup failed: " + r.text);
    }
    reactor_.cancel_timer(img.start_timer);
    img.uffd.reset(m.fds[0]);
    m.fds.clear();
    img.uffd_features = r.uffd_features;
    img.kernel_faults = r.kernel_faults;
    if (!img.pidfd.valid()) {
        img.pidfd.reset(int(syscall(SYS_pidfd_open, img.pid, 0)));
        ProcId proc = p.id;
        if (img.pidfd.valid())
            (void)reactor_.add_fd(img.pidfd.get(), EPOLLIN, [this, proc](uint32_t) { on_image_exit(proc); });
    }
    img.pagemap.reset(::open(("/proc/" + std::to_string(img.pid) + "/pagemap").c_str(), O_RDONLY | O_CLOEXEC));
    if (!img.pidfd.valid() || !img.pagemap.valid())
        return fail_image_create(p.id, "cannot open pidfd/pagemap for image (daemon must be privileged)");
    ProcId proc = p.id;
    Status st = reactor_.add_fd(img.uffd.get(), EPOLLIN, [this, proc](uint32_t) { on_uffd(proc); });
    if (!st.is_ok()) return fail_image_create(p.id, st.str());
    img.ready = true;
    // The final page is reserved for testing page removal and holds no data.
    if (img.backing.valid()) {
        img.revocation = Image::Revocation::Shmem;
    } else {
        auto tail = p.va_base + p.va_size - kPageSize;
        img.revocation = Image::Revocation::Userfaultfd;
        if (!remote_zap(img, tail, kPageSize)) {
            img.revocation = Image::Revocation::ProcessMadvise;
            if (!remote_zap(img, tail, kPageSize)) img.revocation = Image::Revocation::Cooperative;
        }
    }
    const bool direct = img.revocation != Image::Revocation::Cooperative;

    ImageMsg rep;
    rep.proc = p.id;
    rep.pid = img.pid;
    rep.uffd_features = img.uffd_features;
    rep.flags = uint8_t((img.kernel_faults ? kImageKernelFaults : 0) | (direct ? kImageRemoteZap : 0));
    rep.text = cfg_.hostname;
    hooks_.to_coordinator(make_msg(MsgType::ImageReady, rep, img.create_req, inc_));
    const char* method = "cooperative unmap";
    switch (img.revocation) {
    case Image::Revocation::Cooperative: break;
    case Image::Revocation::Shmem: method = "shmem hole punch"; break;
    case Image::Revocation::Userfaultfd: method = "userfaultfd eviction"; break;
    case Image::Revocation::ProcessMadvise: method = "process_madvise"; break;
    }
    LOG_INFO("proc %" PRIu64 ": image pid %d ready (kernel-mode faults: %s, revocation: %s)",
             p.id, img.pid, img.kernel_faults ? "yes" : "NO", method);
    // Faults may already be queued on the descriptor.
    on_uffd(proc);
}

void NodeCore::kill_image(Image& img) {
    if (img.dead) return;
    if (img.pidfd.valid()) syscall(SYS_pidfd_send_signal, img.pidfd.get(), SIGKILL, nullptr, 0);
    else if (img.pid > 0) ::kill(img.pid, SIGKILL);
}

void NodeCore::on_image_closed(ProcId proc, StreamChannel* chan) {
    Proc* p = find_proc(proc);
    if (!p || !p->image || p->image->chan.get() != chan) return;
    // The runtime's control connection is gone: the image can no longer
    // honour revocations, so it must not keep running.
    LOG_DEBUG("proc %" PRIu64 ": runtime connection of pid %d closed; ending the image", proc, p->image->pid);
    kill_image(*p->image);
}

void NodeCore::on_image_exit(ProcId proc) {
    Proc* p = find_proc(proc);
    if (!p || !p->image) return;
    Image& img = *p->image;
    siginfo_t si{};
    if (waitid(static_cast<idtype_t>(P_PIDFD), id_t(img.pidfd.get()), &si, WEXITED | WNOHANG) != 0 || si.si_pid == 0) {
        // Not our child (home images are children of the launcher): the
        // pidfd becoming readable is itself the exit notification.
        si.si_code = CLD_EXITED;
        si.si_status = 0;
        if (img.role == ImageRole::Shadow) return;
    }
    int status = si.si_code == CLD_EXITED ? si.si_status : 128 + si.si_status;
    bool was_ready = img.ready;
    uint64_t req = img.create_req;
    int pid = img.pid;
    LOG_INFO("proc %" PRIu64 ": image pid %d exited (status %d)", proc, pid, status);
    reactor_.del_fd(img.pidfd.get());
    if (img.uffd.valid()) reactor_.del_fd(img.uffd.get());
    reactor_.cancel_timer(img.start_timer);
    img.dead = true;
    for (auto& [id, cb] : img.zaps) (void)id, (void)cb; // continuations die with the image
    // Everything mapped in the image is gone with its address space.
    for (auto it = p->pages.begin(); it != p->pages.end();) {
        LocalPage& lp = it->second;
        if (lp.where == Where::Mapped) {
            account(lp, lp.phys_node, false);
            lp.where = Where::Absent;
            lp.right = Access::None;
        }
        lp.zapping = false;
        lp.requested = Access::None;
        if (lp.where == Where::Absent && lp.export_slot < 0) it = p->pages.erase(it);
        else ++it;
    }
    std::shared_ptr<Image> dead(p->image.release());
    reactor_.post([dead] {}); // destroy outside any handler belonging to it
    ImageMsg r;
    r.proc = proc;
    r.pid = pid;
    r.status = was_ready ? status : (status ? status : EIO);
    r.text = was_ready ? "" : "image exited before becoming ready";
    hooks_.to_coordinator(make_msg(was_ready ? MsgType::ImageExited : MsgType::ImageReady, r, req, inc_));
}

void NodeCore::drop_proc(ProcId id, bool kill) {
    auto it = procs_.find(id);
    if (it == procs_.end()) return;
    Proc& p = it->second;
    if (p.image) {
        Image& img = *p.image;
        if (kill && !img.dead) {
            kill_image(img);
            if (img.role == ImageRole::Shadow && img.pidfd.valid()) {
                // SIGKILL cannot be blocked; wait so that "destroyed" is a fact.
                siginfo_t si{};
                while (waitid(static_cast<idtype_t>(P_PIDFD), id_t(img.pidfd.get()), &si, WEXITED) != 0 && errno == EINTR) {}
            }
        }
        if (img.pidfd.valid()) reactor_.del_fd(img.pidfd.get());
        if (img.uffd.valid()) reactor_.del_fd(img.uffd.get());
        reactor_.cancel_timer(img.start_timer);
        std::shared_ptr<Image> dead(p.image.release());
        reactor_.post([dead] {});
    }
    for (auto& [page, lp] : p.pages) {
        if (lp.where != Where::Absent) account(lp, lp.phys_node, false);
        if (lp.slot >= 0) free_slot(lp.slot);
        if (lp.export_slot >= 0 && lp.export_slot != lp.slot) free_slot(lp.export_slot);
    }
    procs_.erase(it);
}

void NodeCore::reset(const char* why, uint32_t* images_destroyed, uint32_t* contexts_destroyed) {
    *images_destroyed = image_count();
    *contexts_destroyed = context_count();
    if (!procs_.empty())
        LOG_INFO("node reset (%s): destroying %u image(s), %u context(s), %zu process record(s)", why,
                 *images_destroyed, *contexts_destroyed, procs_.size());
    std::vector<ProcId> ids;
    for (auto& [id, p] : procs_) ids.push_back(id);
    for (ProcId id : ids) drop_proc(id, true);
    greeting_.clear();
    resident_.clear();
}

void NodeCore::on_image_msg(Image* img, ProcId proc, Msg&& m) {
    Proc* p = find_proc(proc);
    if (!p) return close_fds(m.fds);
    switch (m.type) {
    case MsgType::RtHello: {
        // Shadow images greet over their inherited socket.
        RtHelloMsg hello;
        RtWelcomeMsg w;
        w.machine = cfg_.self;
        w.hostname = cfg_.hostname;
        if (!parse_msg(m, &hello) || hello.role != ImageRole::Shadow || img->role != ImageRole::Shadow) {
            w.status = EPROTO;
            w.text = "unexpected RT_HELLO";
        } else {
            w.proc = proc;
            w.va_base = p->va_base;
            w.va_size = p->va_size;
            w.canary = p->canary;
            w.uid = p->uid;
            w.gid = p->gid;
        }
        Msg welcome = make_msg(MsgType::RtWelcome, w);
        constexpr uint64_t needed = UFFD_FEATURE_MISSING_SHMEM | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
        if (w.status == 0 && m.req_id == kRtShmemBacking && (res_.uffd_features & needed) == needed &&
            w.va_size >= kPageSize && w.va_size <= uint64_t(std::numeric_limits<off_t>::max())) {
            Fd fd(memfd_create("dnuma-shadow", MFD_CLOEXEC | MFD_ALLOW_SEALING));
            if (fd.valid() && ftruncate(fd.get(), off_t(w.va_size)) == 0 &&
                fcntl(fd.get(), F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0 &&
                fallocate(fd.get(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off_t(w.va_size - kPageSize), kPageSize) == 0) {
                int copy = fcntl(fd.get(), F_DUPFD_CLOEXEC, 0);
                if (copy >= 0) {
                    welcome.fds.push_back(copy);
                    welcome.req_id = kRtShmemBacking;
                    img->backing = std::move(fd);
                    img->backing_base = w.va_base;
                }
            }
        }
        img->chan->send(std::move(welcome));
        break;
    }
    case MsgType::RtReady:
        on_rt_ready(*p, std::move(m));
        break;
    case MsgType::RtZapDone: {
        RtZapMsg z;
        if (!parse_msg(m, &z)) break;
        auto it = img->zaps.find(z.id);
        if (it == img->zaps.end()) break;
        auto cb = std::move(it->second);
        img->zaps.erase(it);
        cb(z.status == 0 ? Status::ok() : Status::err(z.status, "image failed to unmap"));
        break;
    }
    case MsgType::ExecStarted:
    case MsgType::ExecSuspended:
    case MsgType::ExecExited: {
        ExecMsg e;
        if (!parse_msg(m, &e)) break;
        e.proc = proc; // never trust the image's own claim
        e.host = cfg_.hostname;
        if (m.type == MsgType::ExecStarted) {
            if (e.tid > 0) img->tid_ctx[e.tid] = e.ctx;
            auto it = img->ctx_phys.find(e.ctx);
            if (it != img->ctx_phys.end()) e.phys_node = it->second;
        } else {
            img->ctx_phys.erase(e.ctx);
            for (auto it = img->tid_ctx.begin(); it != img->tid_ctx.end();)
                it = it->second == e.ctx ? img->tid_ctx.erase(it) : std::next(it);
        }
        hooks_.to_coordinator(make_msg(m.type, e, m.req_id, inc_));
        break;
    }
    case MsgType::SyscallRequest:
    case MsgType::SyscallComplete: {
        SyscallMsg s;
        if (!parse_msg(m, &s)) break;
        s.proc = proc;
        if (m.type == MsgType::SyscallRequest) s.origin = cfg_.self;
        hooks_.to_coordinator(make_msg(m.type, s, m.req_id, inc_));
        break;
    }
    case MsgType::AppCall: {
        AppCallMsg c;
        if (!parse_msg(m, &c)) break;
        c.proc = proc;
        c.origin = cfg_.self;
        hooks_.to_coordinator(make_msg(MsgType::AppCall, c, m.req_id, inc_));
        break;
    }
    default:
        LOG_WARN("proc %" PRIu64 ": unexpected %s from image", proc, to_string(m.type));
        break;
    }
    close_fds(m.fds);
}

// ============================ coordinator messages ============================

void NodeCore::handle(Msg&& m) {
    switch (m.type) {
    case MsgType::PageGrant:
    case MsgType::PageRevoke:
    case MsgType::PageRelease:
    case MsgType::PageAbort: {
        PageMsg pm;
        if (!parse_msg(m, &pm)) return;
        if (m.type == MsgType::PageGrant) on_grant(pm);
        else if (m.type == MsgType::PageRevoke) on_revoke(pm);
        else if (m.type == MsgType::PageRelease) on_release(pm);
        else on_abort(pm);
        return;
    }
    case MsgType::MemoryAllocate: {
        MemoryMsg mm;
        if (parse_msg(m, &mm)) on_memory_allocate(mm, m.req_id);
        return;
    }
    case MsgType::MemoryRelease: {
        MemoryMsg mm;
        if (parse_msg(m, &mm)) drop_proc(mm.proc, true);
        return;
    }
    case MsgType::ImageCreate: {
        ImageMsg im;
        if (parse_msg(m, &im)) spawn_shadow(im, m.req_id);
        return;
    }
    case MsgType::ImageDestroy: {
        ImageMsg im;
        if (!parse_msg(m, &im)) return;
        Proc* p = find_proc(im.proc);
        if (p && p->image) kill_image(*p->image);
        return;
    }
    case MsgType::ExecCreate:
    case MsgType::ExecStart:
    case MsgType::ExecSuspend:
    case MsgType::ExecMigrate:
    case MsgType::ExecTerminate: {
        ExecMsg e;
        if (!parse_msg(m, &e)) return;
        Proc* p = find_proc(e.proc);
        if (!p || !p->image || !p->image->ready) {
            // Never run something elsewhere just because the target is missing.
            e.status = ESRCH;
            e.host = cfg_.hostname;
            if (m.type == MsgType::ExecCreate || m.type == MsgType::ExecMigrate)
                hooks_.to_coordinator(make_msg(MsgType::ExecStarted, e, m.req_id, inc_));
            return;
        }
        if (m.type == MsgType::ExecCreate || m.type == MsgType::ExecMigrate) {
            int phys = phys_of(e.node);
            e.cpus = cpus_of(phys);
            e.phys_node = phys;
            p->image->ctx_phys[e.ctx] = phys;
        }
        p->image->chan->send(make_msg(m.type, e, m.req_id));
        return;
    }
    case MsgType::SyscallRequest:
    case MsgType::SyscallComplete:
    case MsgType::AppReply: {
        // Relay to the local image of the process named in the body.
        ProcId proc = 0;
        if (m.type == MsgType::AppReply) {
            AppCallMsg c;
            if (!parse_msg(m, &c)) return;
            proc = c.proc;
        } else {
            SyscallMsg s;
            if (!parse_msg(m, &s)) return;
            proc = s.proc;
        }
        Proc* p = find_proc(proc);
        if (p && p->image && !p->image->dead) p->image->chan->send(std::move(m));
        return;
    }
    default:
        LOG_WARN("node: unexpected %s from coordinator", to_string(m.type));
    }
}

// ============================ paging ============================

void NodeCore::on_uffd(ProcId proc) {
    for (;;) {
        Proc* p = find_proc(proc);
        if (!p || !p->image || !p->image->uffd.valid() || p->image->dead) return;
        uffd_msg msg;
        ssize_t n = ::read(p->image->uffd.get(), &msg, sizeof msg);
        if (n < 0) {
            if (errno == EINTR) continue;
            return; // EAGAIN: drained. Anything else: the image is going away.
        }
        if (n != ssize_t(sizeof msg)) return;
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;
        on_fault(*p, msg.arg.pagefault.address, msg.arg.pagefault.flags,
                 (p->image->uffd_features & UFFD_FEATURE_THREAD_ID) ? int(msg.arg.pagefault.feat.ptid) : 0);
    }
}

void NodeCore::segv(Image& img, int tid, uint64_t addr) {
    LOG_WARN("proc %" PRIu64 ": invalid access to distributed address 0x%" PRIx64 " (tid %d): SIGSEGV",
             img.proc, addr, tid);
    metrics_.add("faults_invalid");
    if (tid > 0) syscall(SYS_tgkill, img.pid, tid, SIGSEGV);
    else ::kill(img.pid, SIGSEGV);
    uffd_wake(img, addr & ~(kPageSize - 1));
}

void NodeCore::request_page(Proc& p, PageNo page, LocalPage& lp, Access need, int tid) {
    if (at_least(lp.requested, need)) return;
    lp.requested = need;
    if (!lp.fault_t0) lp.fault_t0 = now_ns();
    PageMsg rq;
    rq.proc = p.id;
    rq.page = page;
    rq.access = need;
    metrics_.add("page_requests");
    // PAGE_REQUEST uses its optional request id to identify the faulting context.
    auto ctx = p.image->tid_ctx.find(tid);
    CtxId id = ctx == p.image->tid_ctx.end() ? 0 : ctx->second;
    hooks_.to_coordinator(make_msg(MsgType::PageRequest, rq, id, inc_));
}

void NodeCore::on_fault(Proc& p, uint64_t addr, uint64_t flags, int tid) {
    Image& img = *p.image;
    metrics_.add("faults_total");
    uint64_t usable_end = p.va_base + p.va_size - kReservedTailBytes;
    if (addr < p.va_base || addr >= usable_end) return segv(img, tid, addr);
    PageNo page = page_of(addr);
    Access need = (flags & (UFFD_PAGEFAULT_FLAG_WRITE | UFFD_PAGEFAULT_FLAG_WP)) ? Access::Write : Access::Read;
    metrics_.add(need == Access::Write ? "faults_write" : "faults_read");
    LocalPage& lp = p.pages[page];
    LOG_TRACE("proc %" PRIu64 ": fault addr=0x%" PRIx64 " flags=0x%" PRIx64 " tid=%d need=%s local: where=%d rw=%d right=%s requested=%s zapping=%d",
              p.id, addr, flags, tid, to_string(need), int(lp.where), int(lp.mapped_rw), to_string(lp.right), to_string(lp.requested), int(lp.zapping));

    if (lp.zapping) {
        // Being revoked right now: the thread stays blocked until the unmap
        // is verified; ask the master to bring the page back afterwards.
        return request_page(p, page, lp, need, tid);
    }
    if (lp.where == Where::Mapped) {
        if (need == Access::Read || lp.mapped_rw) {
            // Stale event: the access is already satisfiable.
            return uffd_wake(img, addr_of(page));
        }
        if (lp.right == Access::Write) {
            // We own the page; only the local mapping is read-only.
            Status st = uffd_wp(img, addr_of(page), false);
            if (!st.is_ok()) {
                LOG_ERROR("proc %" PRIu64 ": %s", p.id, st.str().c_str());
                return kill_image(img);
            }
            lp.mapped_rw = true;
            return;
        }
        return request_page(p, page, lp, need, tid);
    }
    if (lp.where == Where::Parked && at_least(lp.right, need)) {
        Status st = install_from_parked(p, page, lp);
        if (!st.is_ok()) {
            LOG_ERROR("proc %" PRIu64 ": %s", p.id, st.str().c_str());
            kill_image(img);
        }
        return;
    }
    request_page(p, page, lp, need, tid);
}

Status NodeCore::install(Proc& p, PageNo page, LocalPage& lp, const uint8_t* src, bool rw, int phys) {
    Image& img = *p.image;
    // UFFDIO_COPY allocates the page under the calling thread's memory
    // policy, so this is where physical NUMA placement is decided.
    set_thread_preferred_node(phys);
    uffdio_copy cp{};
    cp.dst = addr_of(page);
    cp.src = reinterpret_cast<uint64_t>(src);
    cp.len = kPageSize;
    cp.mode = rw ? 0 : UFFDIO_COPY_MODE_WP;
    for (;;) {
        if (ioctl(img.uffd.get(), UFFDIO_COPY, &cp) == 0) break;
        if (errno == EAGAIN || errno == EINTR) continue;
        // EEXIST means the bytes at this address have unknown provenance.
        if (errno == EEXIST) {
            kill_image(img);
            return Status::err(EEXIST, "unexpected mapping at 0x%" PRIx64, addr_of(page));
        }
        return Status::sys("UFFDIO_COPY page 0x%" PRIx64, addr_of(page));
    }
    if (lp.where != Where::Absent) account(lp, lp.phys_node, false);
    lp.where = Where::Mapped;
    lp.mapped_rw = rw;
    account(lp, phys >= 0 ? phys : -1, true);
    return Status::ok();
}

Status NodeCore::install_from_parked(Proc& p, PageNo page, LocalPage& lp) {
    int32_t slot = lp.slot;
    int phys = lp.phys_node;
    DNUMA_TRY(install(p, page, lp, pool_.addr(uint32_t(slot)), lp.right == Access::Write, phys));
    lp.slot = -1;
    if (lp.export_slot != slot) free_slot(slot); // an exported copy stays until released
    metrics_.add("faults_resolved_local");
    return Status::ok();
}

Status NodeCore::read_image_page(Image& img, uint64_t addr, uint8_t* dst) {
    iovec local{dst, kPageSize};
    iovec remote{reinterpret_cast<void*>(addr), kPageSize};
    ssize_t n = process_vm_readv(img.pid, &local, 1, &remote, 1, 0);
    if (n != ssize_t(kPageSize)) return Status::sys("process_vm_readv pid %d addr 0x%" PRIx64, img.pid, addr);
    return Status::ok();
}

Status NodeCore::uffd_wp(Image& img, uint64_t addr, bool protect) {
    uffdio_writeprotect wp{};
    wp.range.start = addr;
    wp.range.len = kPageSize;
    wp.mode = protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
    for (;;) {
        if (ioctl(img.uffd.get(), UFFDIO_WRITEPROTECT, &wp) == 0) return Status::ok();
        if (errno == EAGAIN || errno == EINTR) continue;
        return Status::sys("UFFDIO_WRITEPROTECT 0x%" PRIx64, addr);
    }
}

void NodeCore::uffd_wake(Image& img, uint64_t addr) {
    uffdio_range r{};
    r.start = addr;
    r.len = kPageSize;
    ioctl(img.uffd.get(), UFFDIO_WAKE, &r);
}

bool NodeCore::page_present(Image& img, uint64_t addr, bool* known) {
    uint64_t ent = 0;
    ssize_t n = pread(img.pagemap.get(), &ent, sizeof ent, off_t(addr / kPageSize * sizeof ent));
    *known = n == ssize_t(sizeof ent);
    return (ent >> 63) & 1 || (ent >> 62) & 1; // present or swapped
}

bool NodeCore::remote_zap(Image& img, uint64_t addr, uint64_t len) {
    if (img.revocation == Image::Revocation::Shmem) {
        if (addr < img.backing_base) { errno = EINVAL; return false; }
        if (fallocate(img.backing.get(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      off_t(addr - img.backing_base), off_t(len)) != 0) return false;
        // A file PTE can retain an uffd-wp marker over the punched hole.
        // Removing the marker makes subsequent writers take a missing fault
        // and allows pagemap verification to distinguish it from swap.
        uffdio_writeprotect clear{{addr, len}, 0};
        return ioctl(img.uffd.get(), UFFDIO_WRITEPROTECT, &clear) == 0;
    }
    if (img.revocation == Image::Revocation::Userfaultfd) {
        uffdio_range range{addr, len};
        return ioctl(img.uffd.get(), DNUMA_UFFD_EVICT, &range) == 0;
    }
    iovec iov{reinterpret_cast<void*>(addr), len};
    return syscall(SYS_process_madvise, img.pidfd.get(), &iov, 1, MADV_DONTNEED, 0) == ssize_t(len);
}

void NodeCore::zap(Image& img, uint64_t addr, uint64_t len, std::function<void(Status)> done) {
    uint64_t id = next_zap_++;
    ProcId proc = img.proc;
    Reactor::TimerId timer = reactor_.add_timer(cfg_.zap_timeout_ms * 1000000ull, [this, proc, id] {
        Proc* p = find_proc(proc);
        if (!p || !p->image || !p->image->zaps.count(id)) return;
        // An image that will not give up a mapping cannot be allowed to run on.
        LOG_ERROR("proc %" PRIu64 ": image did not unmap within the deadline; killing it", proc);
        kill_image(*p->image);
    });
    img.zaps[id] = [this, timer, done = std::move(done)](Status st) {
        reactor_.cancel_timer(timer);
        done(st);
    };
    RtZapMsg z;
    z.id = id;
    z.addr = addr;
    z.len = len;
    img.chan->send(make_msg(MsgType::RtZap, z));
}

void NodeCore::send_complete(const PageMsg& g, int status) {
    PageMsg c = g;
    c.status = status;
    hooks_.to_coordinator(make_msg(MsgType::PageTransferComplete, c, 0, inc_));
}

void NodeCore::on_grant(const PageMsg& g) {
    if (g.status != 0) {
        // The master refused the request: the address is not part of any
        // allocation. Deliver the fault the kernel would have delivered.
        Proc* p = find_proc(g.proc);
        if (!p) return;
        auto it = p->pages.find(g.page);
        if (it != p->pages.end()) {
            it->second.requested = Access::None;
            it->second.fault_t0 = 0;
            maybe_erase(*p, g.page);
        }
        if (p->image && p->image->ready && !p->image->dead) segv(*p->image, 0, addr_of(g.page));
        return;
    }
    Proc& p = get_proc(g.proc);
    LocalPage& lp = p.pages[g.page];
    bool have_image = p.image && p.image->ready && !p.image->dead;
    // Pages are parked in the registered pool only when no local image can
    // hold them; otherwise even unsolicited pages go straight into the
    // image, so parked capacity (bounded by the pool) is not consumed.
    bool park = !have_image;
    int phys = phys_of(g.home_node);
    PAGE_TRACE("PAGE node=%u proc=%" PRIu64 " page=0x%" PRIx64 " grant access=%s source=%s gen=%" PRIu64 "%s",
               cfg_.self, g.proc, addr_of(g.page), to_string(g.access), to_string(g.source), g.gen, park ? " park" : "");

    auto settle = [this](Proc& p, PageNo page, LocalPage& lp, Access granted) {
        if (at_least(granted, lp.requested)) {
            if (lp.fault_t0) metrics_.observe("fault_latency_ns", now_ns() - lp.fault_t0);
            lp.requested = Access::None;
            lp.fault_t0 = 0;
        }
        (void)p;
        (void)page;
    };

    switch (g.source) {
    case GrantSource::Zero: {
        metrics_.add("grants_zero");
        if (park) {
            int64_t slot = pool_.alloc(phys);
            if (slot < 0) return send_complete(g, ENOSPC);
            memset(pool_.addr(uint32_t(slot)), 0, kPageSize);
            lp.right = g.access;
            lp.where = Where::Parked;
            lp.slot = int32_t(slot);
            account(lp, pool_.phys_node_of(uint32_t(slot)), true);
        } else {
            lp.right = g.access;
            Status st = install(p, g.page, lp, zero_page_.data(), g.access == Access::Write, phys);
            if (!st.is_ok()) {
                LOG_ERROR("proc %" PRIu64 ": %s", g.proc, st.str().c_str());
                lp.right = Access::None;
                return send_complete(g, st.code());
            }
        }
        settle(p, g.page, lp, g.access);
        return send_complete(g, 0);
    }
    case GrantSource::Local: {
        metrics_.add("grants_local");
        if (lp.where == Where::Absent) {
            LOG_ERROR("proc %" PRIu64 ": local grant for page 0x%" PRIx64 " we do not hold", g.proc, addr_of(g.page));
            return send_complete(g, ENOENT);
        }
        const Access previous = lp.right;
        lp.right = g.access;
        if (lp.where == Where::Mapped && g.access == Access::Write && !lp.mapped_rw &&
            lp.requested == Access::Write) {
            Status st = uffd_wp(*p.image, addr_of(g.page), false);
            if (!st.is_ok()) {
                lp.right = previous;
                return send_complete(g, st.code());
            }
            lp.mapped_rw = true;
        } else if (lp.where == Where::Mapped) {
            uffd_wake(*p.image, addr_of(g.page));
        } else if (lp.where == Where::Parked && have_image && lp.requested != Access::None) {
            Status st = install_from_parked(p, g.page, lp);
            if (!st.is_ok()) {
                lp.right = previous;
                return send_complete(g, st.code());
            }
        }
        settle(p, g.page, lp, g.access);
        return send_complete(g, 0);
    }
    case GrantSource::Remote:
        break;
    }

    metrics_.add("grants_remote");
    int64_t slot = pool_.alloc(phys);
    if (slot < 0) return send_complete(g, ENOSPC);
    uint64_t t0 = now_ns();
    auto pull = [this, g, slot, park, phys, t0, settle] {
        PeerLink* peer = links_.get(g.src_machine, g.src_inc);
        if (!find_proc(g.proc)) return free_slot(int32_t(slot));
        if (!peer || uint64_t(g.slot) * kPageSize + kPageSize > peer->remote.pool_len) {
            free_slot(int32_t(slot));
            return send_complete(g, EHOSTUNREACH);
        }
        peer->link->read(pool_.addr(uint32_t(slot)), peer->pool_mr->lkey,
                         peer->remote.pool_base + uint64_t(g.slot) * kPageSize, peer->remote.pool_rkey,
                         uint32_t(kPageSize), [this, g, slot, park, phys, t0, settle](Status st, uint64_t lat) {
            Proc* p = find_proc(g.proc);
            if (!p) return free_slot(int32_t(slot)); // process was destroyed meanwhile
            if (!st.is_ok()) {
                LOG_WARN("proc %" PRIu64 ": page pull from machine %u failed: %s", g.proc, g.src_machine, st.str().c_str());
                metrics_.add("page_pull_failures");
                free_slot(int32_t(slot));
                return send_complete(g, EIO);
            }
            metrics_.observe("rdma_read_ns", lat);
            metrics_.add("rdma_read_bytes", kPageSize);
            metrics_.add("page_transfers_in");
            LocalPage& lp = p->pages[g.page];
            bool have_image = p->image && p->image->ready && !p->image->dead;
            lp.right = g.access;
            if (!have_image) {
                if (lp.where != Where::Absent) account(lp, lp.phys_node, false);
                lp.where = Where::Parked;
                lp.slot = int32_t(slot);
                account(lp, pool_.phys_node_of(uint32_t(slot)), true);
            } else {
                Status ist = install(*p, g.page, lp, pool_.addr(uint32_t(slot)), g.access == Access::Write, phys);
                free_slot(int32_t(slot));
                if (!ist.is_ok()) {
                    LOG_ERROR("proc %" PRIu64 ": %s", g.proc, ist.str().c_str());
                    lp.right = Access::None;
                    return send_complete(g, ist.code());
                }
            }
            settle(*p, g.page, lp, g.access);
            metrics_.observe("page_pull_total_ns", now_ns() - t0);
            send_complete(g, 0);
        });
    };
    if (cfg_.test_pull_delay_ms) reactor_.add_timer(cfg_.test_pull_delay_ms * 1000000ull, pull);
    else pull();
}

void NodeCore::finish_revoke(ProcId proc, PageNo page, Generation gen, RevokeMode mode, int32_t export_slot,
                             uint64_t t0, Status st) {
    PageMsg r;
    r.proc = proc;
    r.page = page;
    r.gen = gen;
    r.mode = mode;
    r.status = st.is_ok() ? 0 : st.code();
    r.slot = export_slot >= 0 ? uint32_t(export_slot) : 0;
    r.src_machine = cfg_.self;
    r.src_inc = inc_;
    metrics_.observe("revoke_latency_ns", now_ns() - t0);
    hooks_.to_coordinator(make_msg(MsgType::PageRevoked, r, 0, inc_));
}

void NodeCore::on_revoke(const PageMsg& r) {
    uint64_t t0 = now_ns();
    Proc* p = find_proc(r.proc);
    auto it = p ? p->pages.find(r.page) : decltype(p->pages.begin()){};
    if (!p || it == p->pages.end() || it->second.right == Access::None || it->second.where == Where::Absent) {
        LOG_ERROR("proc %" PRIu64 ": revoke for page 0x%" PRIx64 " that this machine does not hold", r.proc, addr_of(r.page));
        return finish_revoke(r.proc, r.page, r.gen, r.mode, -1, t0, Status::err(ENOENT, "page not held"));
    }
    LocalPage& lp = it->second;
    bool want_export = r.mode != RevokeMode::Invalidate;
    bool invalidate = r.mode == RevokeMode::InvalidateExport || r.mode == RevokeMode::Invalidate;
    metrics_.add("revokes");
    PAGE_TRACE("PAGE node=%u proc=%" PRIu64 " page=0x%" PRIx64 " revoke mode=%s gen=%" PRIu64 " local=%s", cfg_.self,
               r.proc, addr_of(r.page), to_string(r.mode), r.gen, lp.where == Where::Parked ? "parked" : (lp.mapped_rw ? "mapped-rw" : "mapped-ro"));

    // A mapped page needs a staging slot for its exported copy. Wait for one
    // before touching any state, so a retry starts from scratch.
    if (want_export && lp.where == Where::Mapped && pool_.slots_used() == pool_.slots_total()) {
        metrics_.add("revoke_waits_for_slot");
        pool_.when_available([this, r] { on_revoke(r); });
        return;
    }

    if (lp.where == Where::Parked) {
        int32_t exp = -1;
        if (invalidate) {
            account(lp, lp.phys_node, false);
            if (want_export) exp = lp.slot; // the parked slot becomes the staging copy
            else free_slot(lp.slot);
            lp.slot = -1;
            lp.where = Where::Absent;
            lp.right = Access::None;
        } else {
            exp = lp.slot; // served straight from the parked copy
            lp.right = Access::Read;
        }
        lp.export_slot = exp;
        finish_revoke(r.proc, r.page, r.gen, r.mode, exp, t0, Status::ok());
        if (exp < 0) maybe_erase(*p, r.page);
        return;
    }

    Image& img = *p->image;
    uint64_t addr = addr_of(r.page);

    if (!invalidate) {
        // Downgrade / export: writers are stopped by write protection (the
        // kernel flushes TLBs), then the now-stable contents are copied out.
        if (lp.mapped_rw) {
            Status st = uffd_wp(img, addr, true);
            if (!st.is_ok()) return finish_revoke(r.proc, r.page, r.gen, r.mode, -1, t0, st);
            lp.mapped_rw = false;
        }
        lp.right = Access::Read;
        int64_t slot = pool_.alloc(lp.phys_node);
        Status st = slot < 0 ? Status::err(ENOSPC, "no staging slot") : read_image_page(img, addr, pool_.addr(uint32_t(slot)));
        if (!st.is_ok()) {
            free_slot(int32_t(slot));
            return finish_revoke(r.proc, r.page, r.gen, r.mode, -1, t0, st);
        }
        lp.export_slot = int32_t(slot);
        return finish_revoke(r.proc, r.page, r.gen, r.mode, int32_t(slot), t0, Status::ok());
    }

    // ---- invalidation: all access, including read access, must end ----
    lp.right = Access::None;
    int64_t slot = -1;
    if (want_export) {
        slot = pool_.alloc(lp.phys_node);
        if (slot < 0) return finish_revoke(r.proc, r.page, r.gen, r.mode, -1, t0, Status::err(ENOSPC, "no staging slot"));
    }

    // Writers are stopped first, by the kernel, at our request: after this
    // write-protect (which flushes TLBs) the image cannot modify the page
    // any more, so the copy taken next is final.
    if (lp.mapped_rw) {
        Status st = uffd_wp(img, addr, true);
        if (!st.is_ok()) {
            free_slot(int32_t(slot));
            return finish_revoke(r.proc, r.page, r.gen, r.mode, -1, t0, st);
        }
        lp.mapped_rw = false;
    }
    if (want_export) {
        Status st = read_image_page(img, addr, pool_.addr(uint32_t(slot)));
        if (!st.is_ok()) {
            free_slot(int32_t(slot));
            return finish_revoke(r.proc, r.page, r.gen, r.mode, -1, t0, st);
        }
    }
    lp.zapping = true;
    ProcId proc = r.proc;
    PageNo page = r.page;
    Generation gen = r.gen;
    RevokeMode mode = r.mode;
    // Now the mapping itself must go, so that not even a stale read is
    // possible. If the kernel lets the pager unmap directly, do that;
    // otherwise the image's runtime does it (MADV_DONTNEED: PTE removal and
    // TLB shootdown on every CPU running the address space). Either way the
    // result is *verified* through /proc/pid/pagemap before the revocation
    // is acknowledged, and an image that will not let go is killed.
    auto verify = [this, proc, page, gen, mode, slot, t0, want_export](Status st) {
        Proc* p = find_proc(proc);
        auto it = p ? p->pages.find(page) : decltype(p->pages.begin()){};
        if (!p || !p->image || it == p->pages.end()) return free_slot(int32_t(slot));
        LocalPage& lp = it->second;
        Image& img = *p->image;
        uint64_t addr = addr_of(page);
        bool known = false;
        bool present = page_present(img, addr, &known);
        if (!st.is_ok() || !known || present) {
            // The mapping is still there (or we cannot tell). Revocation
            // must not be acknowledged; the image is destroyed instead.
            LOG_ERROR("proc %" PRIu64 ": page 0x%" PRIx64 " still mapped after unmap request; killing image", proc, addr);
            metrics_.add("revoke_verify_failures");
            kill_image(img);
            free_slot(int32_t(slot));
            lp.zapping = false;
            return finish_revoke(proc, page, gen, mode, -1, t0, Status::err(EIO, "unmap not verified"));
        }
        metrics_.add("revoke_verified");
        lp.zapping = false;
        account(lp, lp.phys_node, false);
        lp.where = Where::Absent;
        if (want_export) lp.export_slot = int32_t(slot);
        uffd_wake(img, addr);
        finish_revoke(proc, page, gen, mode, want_export ? int32_t(slot) : -1, t0, Status::ok());
        if (!want_export) maybe_erase(*p, page);
    };
    if (img.revocation != Image::Revocation::Cooperative) {
        if (remote_zap(img, addr, kPageSize)) {
            metrics_.add("revoke_remote_zap");
            if (img.revocation == Image::Revocation::Shmem) metrics_.add("revoke_shmem");
            return verify(Status::ok());
        }
        // MADV_DONTNEED in the application would leave the shmem cache
        // populated, allowing stale bytes to be faulted straight back in.
        if (img.revocation == Image::Revocation::Shmem)
            return verify(Status::sys("punching shadow page 0x%" PRIx64, addr));
    }
    metrics_.add("revoke_coop");
    zap(img, addr, kPageSize, verify);
}

void NodeCore::on_release(const PageMsg& r) {
    Proc* p = find_proc(r.proc);
    if (!p) return;
    auto it = p->pages.find(r.page);
    if (it == p->pages.end()) return;
    LocalPage& lp = it->second;
    if (lp.export_slot >= 0 && lp.export_slot != lp.slot) free_slot(lp.export_slot);
    lp.export_slot = -1;
    metrics_.add("page_transfers_out");
    maybe_erase(*p, r.page);
}

void NodeCore::on_abort(const PageMsg& r) {
    Proc* p = find_proc(r.proc);
    if (!p) return;
    auto it = p->pages.find(r.page);
    if (it == p->pages.end()) return;
    LocalPage& lp = it->second;
    metrics_.add("page_transfer_aborts");
    if (lp.where == Where::Absent && lp.export_slot >= 0) {
        // The transfer never completed: the staged copy is still the
        // authoritative page. Keep it, parked, with full ownership.
        lp.slot = lp.export_slot;
        lp.export_slot = -1;
        lp.where = Where::Parked;
        lp.right = Access::Write;
        account(lp, pool_.phys_node_of(uint32_t(lp.slot)), true);
        PAGE_TRACE("PAGE node=%u proc=%" PRIu64 " page=0x%" PRIx64 " abort: staged copy re-adopted as parked owner gen=%" PRIu64,
                   cfg_.self, r.proc, addr_of(r.page), r.gen);
        return;
    }
    if (lp.export_slot >= 0 && lp.export_slot != lp.slot) free_slot(lp.export_slot);
    lp.export_slot = -1;
    if (r.flags & kPageFlagRestoreWrite) lp.right = Access::Write;
}

void NodeCore::on_memory_allocate(const MemoryMsg& mm, uint64_t req) {
    // Populate: materialize zero-filled pages parked on this machine so the
    // allocation is physically backed here before anything touches it.
    Proc& p = get_proc(mm.proc);
    int phys = phys_of(mm.node);
    MemoryMsg rep = mm;
    rep.count = 0;
    for (uint64_t i = 0; i < mm.count; ++i) {
        LocalPage& lp = p.pages[mm.first + i];
        if (lp.where != Where::Absent) continue;
        int64_t slot = pool_.alloc(phys);
        if (slot < 0) {
            rep.status = ENOSPC;
            maybe_erase(p, mm.first + i);
            break;
        }
        memset(pool_.addr(uint32_t(slot)), 0, kPageSize);
        lp.right = Access::Write;
        lp.where = Where::Parked;
        lp.slot = int32_t(slot);
        account(lp, pool_.phys_node_of(uint32_t(slot)), true);
        ++rep.count;
    }
    hooks_.to_coordinator(make_msg(MsgType::MemoryAllocateAck, rep, req, inc_));
}

} // namespace dnuma
