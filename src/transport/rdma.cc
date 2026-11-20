// SPDX-License-Identifier: GPL-2.0-only
#include "transport/rdma.h"

#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#include "common/log.h"
#include "common/sha256.h"
#include "common/util.h"

namespace dnuma {

namespace {

// A frame larger than one message buffer (kMsgBuf) is sent as
// consecutive fragments. Every fragment starts with a 3-byte header:
//   byte 0   flags (kFragMore: more fragments follow; kFragCreditOnly: no data)
//   byte 1-2 credits returned to the peer (little endian)
//
// Flow control is credit based: a side may only send a fragment when it
// knows the peer has a receive buffer posted for it, so a burst can never
// run into receiver-not-ready back-off on the wire. Credits come back
// piggybacked on traffic in the other direction, or in a credit-only
// fragment once enough have accumulated. One credit is always held back for
// that purpose, which is what keeps the scheme deadlock-free.
//
// Buffers are 128 KiB: large enough that a batch of pages or a typical
// read()/write() payload is a single send. (The HCA limit is far higher; the
// size trades registered memory per link - 48 MiB with the counts below -
// against sends per message.)
constexpr uint32_t kMsgBuf = 128 * 1024;
constexpr uint32_t kFragHdr = 3;
constexpr uint8_t kFragMore = 1, kFragCreditOnly = 2;
constexpr uint32_t kCreditThreshold = 32;
constexpr uint32_t kRecvBufs = 256;
constexpr uint32_t kSendBufs = 128;
constexpr uint32_t kMaxOutstandingOps = 128; // further READ/WRITE requests queue in software
constexpr uint32_t kQueueDepth = 256;        // >= kSendBufs + kMaxOutstandingOps, and kRecvBufs
constexpr uint32_t kLinkMagic = 0x4b4c4e44;  // "DNLK"

constexpr uint64_t kWrRecv = 1ull << 56, kWrSend = 2ull << 56, kWrOp = 3ull << 56;
constexpr uint64_t kWrMask = (1ull << 56) - 1;

std::string sockaddr_ip(sockaddr* sa) {
    char buf[INET6_ADDRSTRLEN] = "";
    if (sa && sa->sa_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(sa)->sin_addr, buf, sizeof buf);
    return buf;
}

bool make_sockaddr(const std::string& ip, uint16_t port, sockaddr_in* sa) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &sa->sin_addr) == 1;
}

void ticket_mac(const std::vector<uint8_t>& key, const LinkTicket& t, uint8_t out[16]) {
    static const char label[] = "dnuma-rdma-link";
    std::vector<uint8_t> buf(sizeof label + offsetof(LinkTicket, mac));
    memcpy(buf.data(), label, sizeof label);
    memcpy(buf.data() + sizeof label, &t, offsetof(LinkTicket, mac));
    Digest d = hmac_sha256(key.data(), key.size(), buf.data(), buf.size());
    memcpy(out, d.data(), 16);
}

} // namespace

std::vector<uint8_t> make_link_ticket(const std::vector<uint8_t>& key, uint16_t kind, MachineId src,
                                      Incarnation src_inc, MachineId dst, Incarnation dst_inc) {
    LinkTicket t{};
    t.magic = kLinkMagic;
    t.version = kProtoVersion;
    t.kind = kind;
    t.src = src;
    t.dst = dst;
    t.src_inc = src_inc;
    t.dst_inc = dst_inc;
    ticket_mac(key, t, t.mac);
    std::vector<uint8_t> out(sizeof t);
    memcpy(out.data(), &t, sizeof t);
    return out;
}

bool check_link_ticket(const std::vector<uint8_t>& key, const std::vector<uint8_t>& bytes,
                       uint16_t kind, LinkTicket* out) {
    // The CM may pad private data with zeros; only the leading ticket matters.
    if (bytes.size() < sizeof(LinkTicket)) return false;
    LinkTicket t;
    memcpy(&t, bytes.data(), sizeof t);
    if (t.magic != kLinkMagic || t.version != kProtoVersion || t.kind != kind) return false;
    uint8_t mac[16];
    ticket_mac(key, t, mac);
    if (!mac_equal(mac, t.mac, 16)) return false;
    *out = t;
    return true;
}

// ---------------- RdmaLink ----------------

RdmaLink::RdmaLink(RdmaEnv& env, rdma_cm_id* id) : env_(env), id_(id) {
    peer_ip_ = sockaddr_ip(rdma_get_peer_addr(id));
    if (id->verbs && id->verbs->device) device_ = ibv_get_device_name(id->verbs->device);
}

RdmaLink::~RdmaLink() { teardown(); }

Status RdmaLink::setup_qp() {
    if (!id_->verbs) return Status::err(ENODEV, "rdma id has no device");
    pd_ = ibv_alloc_pd(id_->verbs);
    if (!pd_) return Status::sys("ibv_alloc_pd");
    cc_ = ibv_create_comp_channel(id_->verbs);
    if (!cc_) return Status::sys("ibv_create_comp_channel");
    DNUMA_TRY(set_nonblock(cc_->fd, true));
    cq_ = ibv_create_cq(id_->verbs, int(kQueueDepth * 4), this, cc_, 0);
    if (!cq_) return Status::sys("ibv_create_cq");
    if (ibv_req_notify_cq(cq_, 0) != 0) return Status::sys("ibv_req_notify_cq");

    ibv_qp_init_attr attr{};
    attr.send_cq = cq_;
    attr.recv_cq = cq_;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = kQueueDepth;
    attr.cap.max_recv_wr = kQueueDepth;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    if (rdma_create_qp(id_, pd_, &attr) != 0) return Status::sys("rdma_create_qp");
    qp_created_ = true;

    size_t bytes = size_t(kMsgBuf) * (kRecvBufs + kSendBufs);
    void* mem = nullptr;
    if (posix_memalign(&mem, 4096, bytes) != 0) return Status::err(ENOMEM, "message buffers");
    msg_mem_ = static_cast<uint8_t*>(mem);
    memset(msg_mem_, 0, bytes);
    msg_mr_ = ibv_reg_mr(pd_, msg_mem_, bytes, IBV_ACCESS_LOCAL_WRITE);
    if (!msg_mr_) return Status::sys("ibv_reg_mr(message buffers)");
    for (uint32_t i = 0; i < kRecvBufs; ++i) post_recv(i);
    for (uint32_t i = 0; i < kSendBufs; ++i) free_send_.push_back(i);

    return env_.reactor().add_fd(cc_->fd, EPOLLIN, [this](uint32_t) { on_cq_event(); });
}

void RdmaLink::post_recv(uint32_t idx) {
    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(msg_mem_ + size_t(idx) * kMsgBuf);
    sge.length = kMsgBuf;
    sge.lkey = msg_mr_->lkey;
    ibv_recv_wr wr{}, *bad = nullptr;
    wr.wr_id = kWrRecv | idx;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    if (ibv_post_recv(id_->qp, &wr, &bad) != 0)
        LOG_ERROR("rdma[%s]: ibv_post_recv failed: %s", peer_ip_.c_str(), strerror(errno));
}

void RdmaLink::start(MsgHandler on_msg, DownHandler on_down) {
    on_msg_ = std::move(on_msg);
    on_down_ = std::move(on_down);
    // Completions may already be waiting (the peer can send as soon as its
    // side is established); process them from the reactor.
    std::weak_ptr<bool> alive = alive_;
    env_.reactor().post([this, alive] {
        auto a = alive.lock();
        if (!a || !*a) return;
        while (!early_.empty() && on_msg_) {
            Msg m = std::move(early_.front());
            early_.pop_front();
            on_msg_(std::move(m));
            if (!*a) return;
        }
        on_cq_event();
    });
}

Status RdmaLink::reg(void* addr, size_t len, int access, ibv_mr** out) {
    ibv_mr* mr = ibv_reg_mr(pd_, addr, len, access);
    if (!mr) return Status::sys("ibv_reg_mr(%zu bytes)", len);
    mrs_.push_back(mr);
    *out = mr;
    return Status::ok();
}

void RdmaLink::send(const Msg& m) {
    if (!up_) return;
    Msg copy = m;
    send(std::move(copy));
}

void RdmaLink::send(Msg&& m) {
    if (!up_) return;
    OutFrame frame;
    encode_header(m, frame.hdr);
    frame.payload = std::move(m.payload);
    send_queue_.push_back(std::move(frame));
    send_times_.push_back(now_ns());
    ++stats_.msgs_tx;
    pump_sends();
}

bool RdmaLink::post_fragment(uint32_t idx, uint32_t len) {
    uint8_t* buf = msg_mem_ + size_t(kRecvBufs + idx) * kMsgBuf;
    uint16_t grant = uint16_t(std::min<uint32_t>(rx_grant_pending_, 0xffff));
    rx_grant_pending_ -= grant;
    memcpy(buf + 1, &grant, 2);
    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(buf);
    sge.length = len;
    sge.lkey = msg_mr_->lkey;
    ibv_send_wr wr{}, *bad = nullptr;
    wr.wr_id = kWrSend | idx;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(id_->qp, &wr, &bad) != 0) {
        go_down(Status::sys("ibv_post_send"));
        return false;
    }
    --tx_credits_;
    return true;
}

void RdmaLink::pump_sends() {
    // Data fragments leave one credit in reserve for a credit-only fragment.
    while (up_ && !free_send_.empty() && !send_queue_.empty() && tx_credits_ > 1) {
        const OutFrame& frame = send_queue_.front();
        uint32_t idx = free_send_.back();
        uint8_t* buf = msg_mem_ + size_t(kRecvBufs + idx) * kMsgBuf;
        size_t chunk = std::min<size_t>(kMsgBuf - kFragHdr, frame.size() - send_off_);
        bool more = send_off_ + chunk < frame.size();
        buf[0] = more ? kFragMore : 0;
        {
            // The frame is the header followed by the payload.
            uint8_t* dst = buf + kFragHdr;
            size_t done = 0;
            if (send_off_ < kHeaderSize) {
                done = std::min(chunk, kHeaderSize - send_off_);
                memcpy(dst, frame.hdr + send_off_, done);
            }
            if (done < chunk) memcpy(dst + done, frame.payload.data() + (send_off_ + done - kHeaderSize), chunk - done);
        }
        free_send_.pop_back();
        if (!post_fragment(idx, uint32_t(chunk + kFragHdr))) return;
        send_off_ += chunk;
        if (!more) {
            send_queue_.pop_front();
            send_off_ = 0;
            uint64_t waited = now_ns() - send_times_.front();
            send_times_.pop_front();
            stats_.send_wait_total_ns += waited;
            stats_.send_wait_max_ns = std::max(stats_.send_wait_max_ns, waited);
        }
    }
    if (up_ && !send_queue_.empty()) {
        if (tx_credits_ <= 1) ++stats_.credit_stalls;
        else if (free_send_.empty()) ++stats_.buffer_stalls;
    }
    if (up_ && rx_grant_pending_ >= kCreditThreshold && tx_credits_ >= 1 && !free_send_.empty()) {
        uint32_t idx = free_send_.back();
        free_send_.pop_back();
        msg_mem_[size_t(kRecvBufs + idx) * kMsgBuf] = kFragCreditOnly;
        (void)post_fragment(idx, kFragHdr);
    }
}

Status RdmaLink::post_op(uint64_t id, const Op& op) {
    ibv_sge sge{};
    sge.addr = reinterpret_cast<uint64_t>(op.local);
    sge.length = op.len;
    sge.lkey = op.lkey;
    ibv_send_wr wr{}, *bad = nullptr;
    wr.wr_id = kWrOp | id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = op.opcode;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = op.remote_addr;
    wr.wr.rdma.rkey = op.rkey;
    if (ibv_post_send(id_->qp, &wr, &bad) != 0) return Status::sys("ibv_post_send(rdma op)");
    return Status::ok();
}

void RdmaLink::pump_ops() {
    while (up_ && !op_queue_.empty() && ops_.size() < kMaxOutstandingOps) {
        auto [id, op] = std::move(op_queue_.front());
        op_queue_.pop_front();
        op.t0 = now_ns();
        Status st = post_op(id, op);
        if (!st.is_ok()) {
            OpDone done = std::move(op.done);
            std::shared_ptr<bool> alive = alive_;
            ++stats_.op_errors;
            done(st, 0);
            if (!*alive) return;
            continue;
        }
        ops_.emplace(id, std::move(op));
    }
}

void RdmaLink::read(void* local, uint32_t lkey, uint64_t remote_addr, uint32_t rkey, uint32_t len,
                    OpDone done) {
    if (!up_) {
        done(Status::err(ENOTCONN, "rdma link to %s is down", peer_ip_.c_str()), 0);
        return;
    }
    op_queue_.emplace_back(next_op_++, Op{IBV_WR_RDMA_READ, local, lkey, remote_addr, rkey, len, std::move(done)});
    pump_ops();
}

void RdmaLink::write(void* local, uint32_t lkey, uint64_t remote_addr, uint32_t rkey, uint32_t len,
                     OpDone done) {
    if (!up_) {
        done(Status::err(ENOTCONN, "rdma link to %s is down", peer_ip_.c_str()), 0);
        return;
    }
    op_queue_.emplace_back(next_op_++, Op{IBV_WR_RDMA_WRITE, local, lkey, remote_addr, rkey, len, std::move(done)});
    pump_ops();
}

void RdmaLink::handle_recv(uint32_t idx, uint32_t len) {
    const uint8_t* p = msg_mem_ + size_t(idx) * kMsgBuf;
    if (len < kFragHdr || len > kMsgBuf) {
        go_down(Status::err(EPROTO, "rdma message fragment has invalid length %u", len));
        return;
    }
    std::shared_ptr<bool> alive = alive_;
    uint8_t flags = p[0];
    uint16_t grant;
    memcpy(&grant, p + 1, 2);
    tx_credits_ += grant;
    if (!(flags & kFragCreditOnly)) rx_accum_.insert(rx_accum_.end(), p + kFragHdr, p + len);
    post_recv(idx);
    ++rx_grant_pending_; // the buffer is available to the peer again
    if (flags & kFragCreditOnly) return pump_sends();
    if (rx_accum_.size() > kHeaderSize + size_t(kMaxPayload)) {
        go_down(Status::err(EPROTO, "rdma message exceeds protocol maximum"));
        return;
    }
    if (flags & kFragMore) return pump_sends();
    std::vector<uint8_t> frame;
    frame.swap(rx_accum_);
    WireHeader h;
    std::string why;
    if (frame.size() < kHeaderSize || !decode_header(frame.data(), &h, &why) ||
        frame.size() != kHeaderSize + h.payload_len || h.nfds != 0) {
        go_down(Status::err(EPROTO, "malformed rdma message: %s", why.c_str()));
        return;
    }
    Msg m;
    m.type = static_cast<MsgType>(h.type);
    m.req_id = h.req_id;
    m.inc = h.incarnation;
    m.payload.assign(frame.begin() + kHeaderSize, frame.end());
    ++stats_.msgs_rx;
    if (on_msg_ && early_.empty()) on_msg_(std::move(m));
    else early_.push_back(std::move(m)); // delivered in order by start()
    if (*alive) pump_sends();
}

void RdmaLink::handle_wc(const ibv_wc& wc) {
    uint64_t kind = wc.wr_id & ~kWrMask, idx = wc.wr_id & kWrMask;
    if (wc.status != IBV_WC_SUCCESS) {
        // Flush errors after the link is already down are expected noise.
        if (kind == kWrOp) {
            auto it = ops_.find(idx);
            if (it != ops_.end()) {
                OpDone done = std::move(it->second.done);
                ops_.erase(it);
                ++stats_.op_errors;
                std::shared_ptr<bool> alive = alive_;
                done(Status::err(EIO, "rdma %s failed: %s", "operation", ibv_wc_status_str(wc.status)), 0);
                if (!*alive) return;
            }
        }
        if (up_) go_down(Status::err(EIO, "rdma completion error: %s", ibv_wc_status_str(wc.status)));
        return;
    }
    if (kind == kWrRecv) {
        handle_recv(uint32_t(idx), wc.byte_len);
    } else if (kind == kWrSend) {
        free_send_.push_back(uint32_t(idx));
        pump_sends();
    } else if (kind == kWrOp) {
        auto it = ops_.find(idx);
        if (it == ops_.end()) return;
        Op op = std::move(it->second);
        ops_.erase(it);
        uint64_t lat = now_ns() - op.t0;
        if (op.opcode == IBV_WR_RDMA_READ) {
            ++stats_.reads;
            stats_.read_bytes += op.len;
        } else {
            ++stats_.writes;
            stats_.write_bytes += op.len;
        }
        std::shared_ptr<bool> alive = alive_;
        op.done(Status::ok(), lat);
        if (!*alive) return;
        pump_ops();
    }
}

void RdmaLink::on_cq_event() {
    if (frozen_ || !cq_) return;
    std::shared_ptr<bool> alive = alive_;
    ibv_cq* evcq = nullptr;
    void* evctx = nullptr;
    while (ibv_get_cq_event(cc_, &evcq, &evctx) == 0) ibv_ack_cq_events(evcq, 1);
    // Re-arm before draining so a completion arriving during the drain still
    // raises an event.
    if (ibv_req_notify_cq(cq_, 0) != 0) {
        go_down(Status::sys("ibv_req_notify_cq"));
        return;
    }
    // Process a bounded amount per invocation. Under sustained traffic an
    // unbounded drain would keep this one handler running and starve every
    // other event source of the (single-threaded) daemon.
    ibv_wc wcs[32];
    for (int batches = 0; batches < 2; ++batches) {
        int n = ibv_poll_cq(cq_, 32, wcs);
        if (n < 0) {
            go_down(Status::err(EIO, "ibv_poll_cq failed"));
            return;
        }
        if (n == 0) return;
        for (int i = 0; i < n; ++i) {
            handle_wc(wcs[i]);
            if (!*alive) return;
        }
        if (n < 32) return;
    }
    // More may be waiting. The notification is already re-armed, so come
    // back explicitly after other handlers have had their turn.
    std::weak_ptr<bool> weak = alive_;
    env_.reactor().post([this, weak] {
        auto a = weak.lock();
        if (a && *a) on_cq_event();
    });
}

void RdmaLink::go_down(const Status& st) {
    if (!up_) return;
    up_ = false;
    LOG_INFO("rdma[%s]: link down: %s", peer_ip_.c_str(), st.str().c_str());
    std::shared_ptr<bool> alive = alive_;
    // Fail everything still in flight; nothing more will complete.
    std::vector<OpDone> failed;
    for (auto& [id, op] : ops_) failed.push_back(std::move(op.done));
    for (auto& [id, op] : op_queue_) failed.push_back(std::move(op.done));
    ops_.clear();
    op_queue_.clear();
    send_queue_.clear();
    send_times_.clear();
    for (OpDone& d : failed) {
        ++stats_.op_errors;
        d(Status::err(ENOTCONN, "rdma link went down: %s", st.message().c_str()), 0);
        if (!*alive) return;
    }
    DownHandler h = std::move(on_down_);
    if (h) h(st); // may destroy *this
}

void RdmaLink::teardown() {
    *alive_ = false;
    if (cc_) env_.reactor().del_fd(cc_->fd);
    if (id_ && up_) rdma_disconnect(id_);
    up_ = false;
    if (id_ && qp_created_) rdma_destroy_qp(id_);
    for (ibv_mr* mr : mrs_) ibv_dereg_mr(mr);
    mrs_.clear();
    if (msg_mr_) ibv_dereg_mr(msg_mr_);
    if (cq_) ibv_destroy_cq(cq_);
    if (cc_) ibv_destroy_comp_channel(cc_);
    if (pd_) ibv_dealloc_pd(pd_);
    if (id_) {
        env_.forget(id_);
        rdma_destroy_id(id_);
    }
    free(msg_mem_);
    id_ = nullptr;
    cq_ = nullptr;
    cc_ = nullptr;
    pd_ = nullptr;
    msg_mr_ = nullptr;
    msg_mem_ = nullptr;
}

// ---------------- RdmaEnv ----------------

RdmaEnv::RdmaEnv(Reactor& reactor) : reactor_(reactor) {}

RdmaEnv::~RdmaEnv() {
    pending_.clear();
    if (listen_id_) rdma_destroy_id(listen_id_);
    if (channel_) {
        reactor_.del_fd(channel_->fd);
        rdma_destroy_event_channel(channel_);
    }
}

Status RdmaEnv::init() {
    channel_ = rdma_create_event_channel();
    if (!channel_) return Status::sys("rdma_create_event_channel (is an RDMA device present?)");
    DNUMA_TRY(set_nonblock(channel_->fd, true));
    return reactor_.add_fd(channel_->fd, EPOLLIN, [this](uint32_t) { on_cm_event(); });
}

Status RdmaEnv::listen(const std::string& ip, uint16_t port, AcceptFn accept, uint16_t* bound_port) {
    sockaddr_in sa;
    if (!make_sockaddr(ip, port, &sa)) return Status::err(EINVAL, "bad RDMA listen address '%s'", ip.c_str());
    if (rdma_create_id(channel_, &listen_id_, nullptr, RDMA_PS_TCP) != 0) return Status::sys("rdma_create_id");
    if (rdma_bind_addr(listen_id_, reinterpret_cast<sockaddr*>(&sa)) != 0)
        return Status::sys("rdma_bind_addr %s:%u", ip.c_str(), port);
    if (rdma_listen(listen_id_, 16) != 0) return Status::sys("rdma_listen");
    if (bound_port) *bound_port = ntohs(rdma_get_src_port(listen_id_));
    accept_ = std::move(accept);
    return Status::ok();
}

void RdmaEnv::connect(const std::string& ip, uint16_t port, std::vector<uint8_t> priv,
                      uint64_t timeout_ms, ConnectDone done) {
    sockaddr_in sa;
    if (!make_sockaddr(ip, port, &sa)) {
        done(Status::err(EINVAL, "bad RDMA address '%s'", ip.c_str()), nullptr, {});
        return;
    }
    rdma_cm_id* id = nullptr;
    if (rdma_create_id(channel_, &id, nullptr, RDMA_PS_TCP) != 0) {
        done(Status::sys("rdma_create_id"), nullptr, {});
        return;
    }
    Pending& p = pending_[id];
    p.active = true;
    p.connect_done = std::move(done);
    p.priv = std::move(priv);
    p.timer = reactor_.add_timer(timeout_ms * 1000000ull, [this, id, ip, port] {
        finish_pending(id, Status::err(ETIMEDOUT, "rdma connect to %s:%u timed out", ip.c_str(), port), {});
    });
    if (rdma_resolve_addr(id, nullptr, reinterpret_cast<sockaddr*>(&sa), int(timeout_ms)) != 0)
        finish_pending(id, Status::sys("rdma_resolve_addr %s", ip.c_str()), {});
}

void RdmaEnv::finish_pending(rdma_cm_id* id, Status st, std::vector<uint8_t> reply) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    Pending p = std::move(it->second);
    pending_.erase(it);
    reactor_.cancel_timer(p.timer);
    std::unique_ptr<RdmaLink> link = std::move(p.link);
    if (st.is_ok() && link) {
        link->up_ = true;
        links_[id] = link.get();
    } else {
        if (st.is_ok()) st = Status::err(EIO, "rdma connection has no link");
        if (link) link.reset(); // destroys the id as well
        else rdma_destroy_id(id);
    }
    if (p.active) {
        if (p.connect_done) p.connect_done(st, std::move(link), std::move(reply));
    } else if (p.accept_ready) {
        p.accept_ready(st, std::move(link));
    }
}

void RdmaEnv::on_cm_event() {
    rdma_cm_event* ev = nullptr;
    while (rdma_get_cm_event(channel_, &ev) == 0) {
        rdma_cm_event_type type = ev->event;
        rdma_cm_id* id = ev->id;
        int status = ev->status;
        std::vector<uint8_t> priv;
        if ((type == RDMA_CM_EVENT_CONNECT_REQUEST || type == RDMA_CM_EVENT_ESTABLISHED) &&
            ev->param.conn.private_data && ev->param.conn.private_data_len) {
            const uint8_t* p = static_cast<const uint8_t*>(ev->param.conn.private_data);
            priv.assign(p, p + ev->param.conn.private_data_len);
        }
        // Acknowledge before acting: destroying an id blocks until all of its
        // events have been acknowledged.
        rdma_ack_cm_event(ev);
        if (ignore_events_) continue;

        rdma_conn_param param{};
        param.responder_resources = 16;
        param.initiator_depth = 16;
        param.retry_count = 7;
        param.rnr_retry_count = 7; // retry forever on receiver-not-ready

        switch (type) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            if (pending_.count(id) && rdma_resolve_route(id, 2000) != 0)
                finish_pending(id, Status::sys("rdma_resolve_route"), {});
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED: {
            auto it = pending_.find(id);
            if (it == pending_.end()) break;
            it->second.link.reset(new RdmaLink(*this, id));
            Status st = it->second.link->setup_qp();
            if (st.is_ok()) {
                param.private_data = it->second.priv.data();
                param.private_data_len = uint8_t(it->second.priv.size());
                if (rdma_connect(id, &param) != 0) st = Status::sys("rdma_connect");
            }
            if (!st.is_ok()) finish_pending(id, st, {});
            break;
        }
        case RDMA_CM_EVENT_CONNECT_REQUEST: {
            Incoming in;
            in.priv = std::move(priv);
            in.peer_ip = sockaddr_ip(rdma_get_peer_addr(id));
            std::vector<uint8_t> reply;
            LinkReady ready;
            if (!accept_ || !accept_(in, &reply, &ready)) {
                LOG_WARN("rdma: rejected connection request from %s", in.peer_ip.c_str());
                rdma_reject(id, nullptr, 0);
                rdma_destroy_id(id);
                break;
            }
            Pending& p = pending_[id];
            p.active = false;
            p.accept_ready = std::move(ready);
            p.link.reset(new RdmaLink(*this, id));
            Status st = p.link->setup_qp();
            if (st.is_ok()) {
                param.private_data = reply.data();
                param.private_data_len = uint8_t(reply.size());
                if (rdma_accept(id, &param) != 0) st = Status::sys("rdma_accept");
            }
            if (!st.is_ok()) {
                finish_pending(id, st, {});
                break;
            }
            p.timer = reactor_.add_timer(10ull * 1000000000ull, [this, id] {
                finish_pending(id, Status::err(ETIMEDOUT, "rdma accept never completed"), {});
            });
            break;
        }
        case RDMA_CM_EVENT_ESTABLISHED:
            finish_pending(id, Status::ok(), std::move(priv));
            break;
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_DISCONNECTED:
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_ADDR_CHANGE: {
            Status st = Status::err(type == RDMA_CM_EVENT_REJECTED ? ECONNREFUSED : ECONNRESET,
                                    "rdma cm event %s (status %d)", rdma_event_str(type), status);
            if (pending_.count(id)) {
                finish_pending(id, st, {});
            } else {
                auto it = links_.find(id);
                if (it != links_.end()) it->second->go_down(st); // may destroy the link
            }
            break;
        }
        default:
            break; // TIMEWAIT_EXIT and multicast events are not interesting
        }
    }
}

} // namespace dnuma
