/* SPDX-License-Identifier: GPL-2.0-only */
// RDMA transport: reliable-connected queue pairs established with librdmacm.
//
// One RdmaLink exists per (peer machine, peer incarnation). Each link owns a
// private protection domain, so every memory registration made for a link is
// reachable only through that link's queue pair: destroying the link revokes
// all RDMA access its peer ever had, and a stale incarnation can never reach
// memory registered for a newer one.
//
// A link carries two kinds of traffic:
//   * messages  - ordinary protocol frames over SEND/RECV (fragmented, so
//                 there is no message size limit below kMaxPayload);
//   * bulk data - RDMA READ / WRITE against the peer's registered regions.
//
// Everything is event driven: the CM event channel and each link's completion
// channel are reactor event sources.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "common/reactor.h"
#include "common/status.h"
#include "common/types.h"
#include "transport/wire.h"

namespace dnuma {

class RdmaEnv;

// Connection-request private data. InfiniBand CM leaves 56 bytes of private
// data for RDMA_PS_TCP requests (a wire-format constraint of the IB CM REQ
// message), so the ticket is a fixed 48-byte structure.
struct LinkTicket {
    uint32_t magic;
    uint16_t version;
    uint16_t kind; // 0 = request, 1 = reply (prevents reflection)
    MachineId src;
    MachineId dst;
    Incarnation src_inc;
    Incarnation dst_inc;
    uint8_t mac[16];
} __attribute__((packed));
static_assert(sizeof(LinkTicket) == 48, "link ticket must fit IB CM private data");

std::vector<uint8_t> make_link_ticket(const std::vector<uint8_t>& key, uint16_t kind, MachineId src,
                                      Incarnation src_inc, MachineId dst, Incarnation dst_inc);
// Verifies the MAC and structure; on success fills *out.
bool check_link_ticket(const std::vector<uint8_t>& key, const std::vector<uint8_t>& bytes,
                       uint16_t kind, LinkTicket* out);

class RdmaLink {
public:
    using MsgHandler = std::function<void(Msg&&)>;
    using DownHandler = std::function<void(const Status&)>;
    using OpDone = std::function<void(Status, uint64_t latency_ns)>;

    ~RdmaLink();
    RdmaLink(const RdmaLink&) = delete;
    RdmaLink& operator=(const RdmaLink&) = delete;

    // The down handler runs at most once, on disconnect or any completion
    // error. It may destroy the link.
    void start(MsgHandler on_msg, DownHandler on_down);
    void send(const Msg& m);
    void send(Msg&& m); // takes the payload over instead of copying it

    // Registers memory in this link's protection domain. The registration
    // lives exactly as long as the link.
    Status reg(void* addr, size_t len, int access, ibv_mr** out);

    void read(void* local, uint32_t lkey, uint64_t remote_addr, uint32_t rkey, uint32_t len, OpDone done);
    void write(void* local, uint32_t lkey, uint64_t remote_addr, uint32_t rkey, uint32_t len, OpDone done);

    bool is_up() const { return up_; }
    const std::string& peer_ip() const { return peer_ip_; }
    const std::string& device() const { return device_; }
    size_t pending_ops() const { return ops_.size() + op_queue_.size(); }

    struct Stats {
        uint64_t msgs_tx = 0, msgs_rx = 0, reads = 0, writes = 0, read_bytes = 0, write_bytes = 0,
                 op_errors = 0;
        // Time messages spent queued here before their last fragment was
        // handed to the HCA (waiting for send buffers or for credits).
        uint64_t send_wait_total_ns = 0, send_wait_max_ns = 0, credit_stalls = 0, buffer_stalls = 0;
    };
    const Stats& stats() const { return stats_; }

    // Fault injection: keep the queue pair alive but stop processing, to
    // model a peer that has gone silent.
    void test_freeze(bool on) { frozen_ = on; }
    // Processes whatever completed while frozen.
    void test_kick() { on_cq_event(); }

private:
    friend class RdmaEnv;
    RdmaLink(RdmaEnv& env, rdma_cm_id* id);
    Status setup_qp();
    void on_cq_event();
    void handle_wc(const ibv_wc& wc);
    void handle_recv(uint32_t idx, uint32_t len);
    void pump_sends();
    bool post_fragment(uint32_t idx, uint32_t len);
    void pump_ops();
    void post_recv(uint32_t idx);
    void go_down(const Status& st);
    void teardown();

    struct Op {
        ibv_wr_opcode opcode;
        void* local;
        uint32_t lkey;
        uint64_t remote_addr;
        uint32_t rkey;
        uint32_t len;
        OpDone done;
        uint64_t t0 = 0;
    };
    Status post_op(uint64_t id, const Op& op);

    RdmaEnv& env_;
    rdma_cm_id* id_ = nullptr;
    ibv_pd* pd_ = nullptr;
    ibv_comp_channel* cc_ = nullptr;
    ibv_cq* cq_ = nullptr;
    bool qp_created_ = false;
    bool up_ = false;
    bool frozen_ = false;
    std::string peer_ip_, device_;

    uint8_t* msg_mem_ = nullptr;
    ibv_mr* msg_mr_ = nullptr;
    std::vector<uint32_t> free_send_;
    struct OutFrame {
        uint8_t hdr[kHeaderSize];
        std::vector<uint8_t> payload;
        size_t size() const { return kHeaderSize + payload.size(); }
    };
    std::deque<OutFrame> send_queue_;              // frames awaiting buffers
    std::deque<uint64_t> send_times_;              // when each was queued
    size_t send_off_ = 0;                          // offset into send_queue_.front()
    std::vector<uint8_t> rx_accum_;
    uint32_t tx_credits_ = 256;      // receive buffers the peer has posted for us (kRecvBufs)
    uint32_t rx_grant_pending_ = 0;  // credits we owe the peer
    std::deque<Msg> early_; // messages that arrived before start()

    std::vector<ibv_mr*> mrs_;
    std::unordered_map<uint64_t, Op> ops_;
    std::deque<std::pair<uint64_t, Op>> op_queue_;
    uint64_t next_op_ = 1;

    MsgHandler on_msg_;
    DownHandler on_down_;
    Stats stats_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

class RdmaEnv {
public:
    struct Incoming {
        std::vector<uint8_t> priv;
        std::string peer_ip;
    };
    using LinkReady = std::function<void(Status, std::unique_ptr<RdmaLink>)>;
    // Decide whether to accept a connection request. On acceptance, fill the
    // reply private data and the callback that receives the established link.
    using AcceptFn = std::function<bool(const Incoming&, std::vector<uint8_t>* reply, LinkReady* ready)>;
    using ConnectDone =
        std::function<void(Status, std::unique_ptr<RdmaLink>, std::vector<uint8_t> reply_priv)>;

    explicit RdmaEnv(Reactor& reactor);
    ~RdmaEnv();
    Status init();

    Status listen(const std::string& ip, uint16_t port, AcceptFn accept, uint16_t* bound_port);
    void connect(const std::string& ip, uint16_t port, std::vector<uint8_t> priv, uint64_t timeout_ms,
                 ConnectDone done);

    Reactor& reactor() { return reactor_; }
    // Fault injection: discard connection-manager events, as if the fabric
    // had partitioned without either side being told.
    void test_ignore_events(bool on) { ignore_events_ = on; }

private:
    friend class RdmaLink;
    struct Pending {
        std::unique_ptr<RdmaLink> link;
        ConnectDone connect_done;
        LinkReady accept_ready;
        std::vector<uint8_t> priv;
        Reactor::TimerId timer = 0;
        bool active = false;
    };
    void on_cm_event();
    void finish_pending(rdma_cm_id* id, Status st, std::vector<uint8_t> reply);
    void forget(rdma_cm_id* id) { links_.erase(id); }

    Reactor& reactor_;
    rdma_event_channel* channel_ = nullptr;
    rdma_cm_id* listen_id_ = nullptr;
    AcceptFn accept_;
    std::unordered_map<rdma_cm_id*, Pending> pending_;
    std::unordered_map<rdma_cm_id*, RdmaLink*> links_;
    bool ignore_events_ = false;
};

} // namespace dnuma
