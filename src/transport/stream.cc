// SPDX-License-Identifier: GPL-2.0-only
#include "transport/stream.h"

#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "common/log.h"
#include "common/sha256.h"

namespace dnuma {

const char* to_string(MsgType t) {
    switch (t) {
#define X(n) case MsgType::n: return #n;
        X(Hello) X(HelloAck) X(Auth) X(NodeAttach) X(NodeAttachAck) X(NodeAssign) X(NodeAssignAck)
        X(NodeDetach) X(NodeDetachAck) X(NodeDrain) X(NodeFence) X(NodeFenceAck) X(ResourceReport) X(ViewSample)
        X(Heartbeat) X(HeartbeatAck) X(PeerExpect) X(PeerConnect) X(PeerFence) X(PeerStatus)
        X(FaultInject) X(FaultInjectAck) X(LinkHello) X(LinkVerify) X(LinkVerifyAck)
        X(MemoryAllocate) X(MemoryAllocateAck) X(MemoryRelease) X(PageRequest) X(PageGrant)
        X(PageRevoke) X(PageRevoked) X(PageTransferComplete) X(PageRelease) X(PageAbort)
        X(ImageCreate) X(ImageReady) X(ImageExited) X(ImageDestroy) X(ExecCreate) X(ExecStart)
        X(ExecSuspend) X(ExecMigrate) X(ExecTerminate) X(ExecStarted) X(ExecSuspended) X(ExecExited)
        X(SyscallRequest) X(SyscallComplete) X(AppCall) X(AppReply) X(RtHello) X(RtWelcome)
        X(RtReady) X(RtZap) X(RtZapDone) X(RtLease) X(CtlRequest) X(CtlOutput) X(CtlDone) X(XBegin) X(XReady) X(XSyscall) X(XSysRet)
        X(XPageReq) X(XPage) X(XStop) X(XHome) X(XPagePush) X(XHomeDone) X(XKill) X(XGone) X(XClockReq) X(XClock)
#undef X
    }
    return "Unknown";
}

void encode_header(const Msg& m, uint8_t out[kHeaderSize]) {
    WireHeader h{};
    h.magic = kWireMagic;
    h.version = kProtoVersion;
    h.type = static_cast<uint16_t>(m.type);
    h.payload_len = static_cast<uint32_t>(m.payload.size());
    h.nfds = static_cast<uint32_t>(m.fds.size());
    h.req_id = m.req_id;
    h.incarnation = m.inc;
    memcpy(out, &h, sizeof h);
}

bool decode_header(const uint8_t in[kHeaderSize], WireHeader* out, std::string* why) {
    memcpy(out, in, sizeof *out);
    if (out->magic != kWireMagic) {
        *why = "bad magic";
        return false;
    }
    if (out->version < kProtoVersionMin || out->version > kProtoVersion) {
        *why = "unsupported protocol version " + std::to_string(out->version);
        return false;
    }
    if (out->payload_len > kMaxPayload) {
        *why = "payload length " + std::to_string(out->payload_len) + " exceeds protocol maximum";
        return false;
    }
    if (out->nfds > kMaxFdsPerFrame) {
        *why = "too many descriptors in frame";
        return false;
    }
    return true;
}

void close_fds(std::vector<int>& fds) {
    for (int f : fds)
        if (f >= 0) ::close(f);
    fds.clear();
}

namespace {

void frame_mac(const std::vector<uint8_t>& key, uint64_t seq, const uint8_t* hdr, const uint8_t* payload,
               size_t len, uint8_t out[kMacSize]) {
    // HMAC(key, seq | header | payload), truncated to 128 bits.
    std::vector<uint8_t> buf(8 + kHeaderSize + len);
    memcpy(buf.data(), &seq, 8);
    memcpy(buf.data() + 8, hdr, kHeaderSize);
    if (len) memcpy(buf.data() + 8 + kHeaderSize, payload, len);
    Digest d = hmac_sha256(key.data(), key.size(), buf.data(), buf.size());
    memcpy(out, d.data(), kMacSize);
}

// sendmsg with optional SCM_RIGHTS. Returns bytes written or -1.
ssize_t send_with_fds(int fd, const uint8_t* data, size_t len, const std::vector<int>& fds) {
    iovec iov{const_cast<uint8_t*>(data), len};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerFrame)];
    if (!fds.empty()) {
        memset(cbuf, 0, sizeof cbuf);
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
    }
    return ::sendmsg(fd, &mh, MSG_NOSIGNAL);
}

// recvmsg collecting any SCM_RIGHTS descriptors into `fds`.
ssize_t recv_with_fds(int fd, uint8_t* data, size_t len, std::deque<int>* fds) {
    iovec iov{data, len};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerFrame * 4)];
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof cbuf;
    ssize_t n = ::recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
    if (n < 0) return n;
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int f;
            memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
            fds->push_back(f);
        }
    }
    return n;
}

std::vector<uint8_t> serialize(const Msg& m) {
    std::vector<uint8_t> out(kHeaderSize + m.payload.size());
    encode_header(m, out.data());
    if (!m.payload.empty()) memcpy(out.data() + kHeaderSize, m.payload.data(), m.payload.size());
    return out;
}

} // namespace

// ---------------- StreamChannel ----------------

StreamChannel::StreamChannel(Reactor& reactor, Fd fd, bool pass_fds)
    : reactor_(reactor), fd_(std::move(fd)), pass_fds_(pass_fds) {
    (void)set_nonblock(fd_.get(), true);
}

StreamChannel::~StreamChannel() { close(); }

void StreamChannel::start(MsgHandler on_msg, CloseHandler on_close) {
    on_msg_ = std::move(on_msg);
    on_close_ = std::move(on_close);
    Status st = reactor_.add_fd(fd_.get(), EPOLLIN | EPOLLRDHUP, [this](uint32_t ev) { on_events(ev); });
    if (!st.is_ok()) LOG_ERROR("stream: %s", st.str().c_str());
}

void StreamChannel::close() {
    *alive_ = false;
    alive_ = std::make_shared<bool>(true);
    if (fd_.valid()) {
        reactor_.del_fd(fd_.get());
        fd_.reset();
    }
    for (int f : rfds_) ::close(f);
    rfds_.clear();
    for (auto& b : wq_) close_fds(b.fds);
    wq_.clear();
    wq_bytes_ = 0;
}

void StreamChannel::enable_mac(std::vector<uint8_t> key) {
    mac_ = true;
    mac_key_ = std::move(key);
    tx_seq_ = rx_seq_ = 0;
}

void StreamChannel::fail(const Status& st) {
    CloseHandler h = std::move(on_close_);
    close();
    if (h) h(st); // may destroy *this
}

void StreamChannel::update_interest() {
    bool want = !wq_.empty();
    if (want == want_write_ || !fd_.valid()) return;
    want_write_ = want;
    (void)reactor_.mod_fd(fd_.get(), EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0u));
}

void StreamChannel::send(Msg m) {
    if (!fd_.valid()) {
        close_fds(m.fds);
        return;
    }
    OutBuf b;
    b.data = serialize(m);
    if (mac_) {
        uint8_t tag[kMacSize];
        frame_mac(mac_key_, tx_seq_++, b.data.data(), m.payload.data(), m.payload.size(), tag);
        b.data.insert(b.data.end(), tag, tag + kMacSize);
    }
    b.fds = std::move(m.fds);
    bool was_empty = wq_.empty();
    wq_bytes_ += b.data.size();
    wq_.push_back(std::move(b));
    if (was_empty) {
        Status err;
        if (!do_write(&err)) {
            // Report asynchronously: callers of send() do not expect the
            // channel to disappear underneath them.
            std::weak_ptr<bool> alive = alive_;
            reactor_.post([this, alive, err] {
                auto a = alive.lock();
                if (a && *a) fail(err);
            });
            return;
        }
    }
    update_interest();
}

bool StreamChannel::do_write(Status* err) {
    while (!wq_.empty()) {
        OutBuf& b = wq_.front();
        ssize_t n;
        if (!b.fds.empty() && b.off == 0)
            n = send_with_fds(fd_.get(), b.data.data(), b.data.size(), b.fds);
        else
            n = ::send(fd_.get(), b.data.data() + b.off, b.data.size() - b.off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            *err = Status::sys("send");
            return false;
        }
        if (b.off == 0) close_fds(b.fds); // descriptors travel with the first byte
        b.off += size_t(n);
        tx_bytes_ += uint64_t(n);
        wq_bytes_ -= std::min(wq_bytes_, size_t(n));
        if (b.off == b.data.size()) wq_.pop_front();
    }
    return true;
}

bool StreamChannel::do_read(Status* err) {
    for (;;) {
        size_t old = rbuf_.size();
        rbuf_.resize(old + 65536);
        ssize_t n = pass_fds_ ? recv_with_fds(fd_.get(), rbuf_.data() + old, 65536, &rfds_)
                              : ::recv(fd_.get(), rbuf_.data() + old, 65536, 0);
        if (n < 0) {
            rbuf_.resize(old);
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            *err = Status::sys("recv");
            return false;
        }
        rbuf_.resize(old + size_t(n));
        if (n == 0) {
            *err = Status::err(ECONNRESET, "peer closed connection");
            return false;
        }
        rx_bytes_ += uint64_t(n);
        if (size_t(n) < 65536) return true;
    }
}

bool StreamChannel::parse(Status* err, const std::shared_ptr<bool>& alive) {
    while (*alive) {
        size_t avail = rbuf_.size() - rpos_;
        if (avail < kHeaderSize) break;
        WireHeader h;
        std::string why;
        if (!decode_header(rbuf_.data() + rpos_, &h, &why)) {
            *err = Status::err(EPROTO, "malformed frame: %s", why.c_str());
            return false;
        }
        size_t total = kHeaderSize + h.payload_len + (mac_ ? kMacSize : 0);
        if (avail < total) break;
        const uint8_t* base = rbuf_.data() + rpos_;
        if (mac_) {
            uint8_t tag[kMacSize];
            frame_mac(mac_key_, rx_seq_++, base, base + kHeaderSize, h.payload_len, tag);
            if (!mac_equal(tag, base + kHeaderSize + h.payload_len, kMacSize)) {
                *err = Status::err(EBADMSG, "frame authentication failed");
                return false;
            }
        }
        Msg m;
        m.type = static_cast<MsgType>(h.type);
        m.req_id = h.req_id;
        m.inc = h.incarnation;
        m.payload.assign(base + kHeaderSize, base + kHeaderSize + h.payload_len);
        if (h.nfds) {
            if (!pass_fds_ || rfds_.size() < h.nfds) {
                *err = Status::err(EPROTO, "frame announces %u descriptors, %zu received", h.nfds,
                                   rfds_.size());
                return false;
            }
            for (uint32_t i = 0; i < h.nfds; ++i) {
                m.fds.push_back(rfds_.front());
                rfds_.pop_front();
            }
        }
        rpos_ += total;
        on_msg_(std::move(m)); // may close or destroy the channel
    }
    if (*alive) {
        if (rpos_ == rbuf_.size()) {
            rbuf_.clear();
            rpos_ = 0;
        } else if (rpos_ > (1u << 20)) {
            rbuf_.erase(rbuf_.begin(), rbuf_.begin() + ssize_t(rpos_));
            rpos_ = 0;
        }
    }
    return true;
}

void StreamChannel::on_events(uint32_t ev) {
    std::shared_ptr<bool> alive = alive_;
    Status err;
    if (ev & EPOLLOUT) {
        if (!do_write(&err)) return fail(err);
        update_interest();
        if (wq_.empty()) {
            if (closing_) return close();
            if (on_drain_) {
                on_drain_(); // may send more, close or destroy the channel
                if (!*alive) return;
            }
        }
    }
    if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        bool read_ok = do_read(&err);
        // Deliver everything that arrived before reporting a close.
        Status perr;
        if (!parse(&perr, alive)) {
            if (*alive) fail(perr);
            return;
        }
        if (!*alive) return;
        if (!read_ok) return fail(err);
    }
}

// ---------------- blocking helpers ----------------

Status send_msg_blocking(int fd, const Msg& m) {
    std::vector<uint8_t> data = serialize(m);
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = (off == 0 && !m.fds.empty())
                        ? send_with_fds(fd, data.data(), data.size(), m.fds)
                        : ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::sys("send");
        }
        off += size_t(n);
    }
    return Status::ok();
}

namespace {
Status read_exact(int fd, uint8_t* p, size_t n, std::deque<int>* fds) {
    while (n) {
        ssize_t r = recv_with_fds(fd, p, n, fds);
        if (r < 0) {
            if (errno == EINTR) continue;
            return Status::sys("recv");
        }
        if (r == 0) return Status::err(ECONNRESET, "peer closed connection");
        p += r;
        n -= size_t(r);
    }
    return Status::ok();
}
} // namespace

Status recv_msg_blocking(int fd, Msg* m) {
    std::deque<int> fds;
    auto drop = [&] {
        for (int f : fds) ::close(f);
    };
    uint8_t hdr[kHeaderSize];
    Status st = read_exact(fd, hdr, sizeof hdr, &fds);
    if (!st.is_ok()) return drop(), st;
    WireHeader h;
    std::string why;
    if (!decode_header(hdr, &h, &why)) return drop(), Status::err(EPROTO, "malformed frame: %s", why.c_str());
    m->type = static_cast<MsgType>(h.type);
    m->req_id = h.req_id;
    m->inc = h.incarnation;
    m->payload.resize(h.payload_len);
    m->fds.clear();
    if (h.payload_len) {
        st = read_exact(fd, m->payload.data(), h.payload_len, &fds);
        if (!st.is_ok()) return drop(), st;
    }
    if (fds.size() != h.nfds) return drop(), Status::err(EPROTO, "descriptor count mismatch");
    m->fds.assign(fds.begin(), fds.end());
    return Status::ok();
}

// ---------------- sockets ----------------

Status unix_listen(const std::string& path, int mode, Fd* out) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path) // sockaddr_un is a fixed kernel ABI structure
        return Status::err(ENAMETOOLONG, "socket path too long: %s", path.c_str());
    memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) return Status::sys("socket");
    ::unlink(path.c_str());
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0)
        return Status::sys("bind %s", path.c_str());
    if (::chmod(path.c_str(), mode_t(mode)) != 0) return Status::sys("chmod %s", path.c_str());
    if (::listen(fd.get(), 64) != 0) return Status::sys("listen %s", path.c_str());
    *out = std::move(fd);
    return Status::ok();
}

Status unix_connect(const std::string& path, Fd* out) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path)
        return Status::err(ENAMETOOLONG, "socket path too long: %s", path.c_str());
    memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) return Status::sys("socket");
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0)
        return Status::sys("connect %s", path.c_str());
    *out = std::move(fd);
    return Status::ok();
}

Fd accept_one(int listen_fd, std::string* peer) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&ss), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return Fd();
    if (peer) {
        peer->clear();
        if (ss.ss_family == AF_INET) {
            char buf[INET_ADDRSTRLEN];
            auto* in = reinterpret_cast<sockaddr_in*>(&ss);
            if (inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf))
                *peer = std::string(buf) + ":" + std::to_string(ntohs(in->sin_port));
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
    }
    return Fd(fd);
}

} // namespace dnuma
