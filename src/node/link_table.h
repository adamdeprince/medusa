/* SPDX-License-Identifier: GPL-2.0-only */
// Per-peer RDMA links for one node. Shared by the master (peers = slaves) and
// the agents (peers = the master and other slaves).
//
// When a link comes up, the node registers its page pool (remote read) and a
// small per-link inbox (remote read+write) in the link's own protection
// domain, then exchanges LINK_HELLO so each side learns the other's keys.
// Page data is always *pulled* with RDMA READ from the pool; the inbox is the
// only remotely writable memory and is private to that one link.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "common/metrics.h"
#include "node/page_pool.h"
#include "transport/messages.h"
#include "transport/rdma.h"

namespace dnuma {

inline constexpr size_t kInboxBytes = 64 * 1024; // self-test area, fixed by protocol
inline constexpr size_t kVerifyBytes = 16 * 1024;

struct PeerLink {
    MachineId machine = kNoMachine;
    Incarnation inc = 0;          // peer's incarnation this link belongs to
    std::unique_ptr<RdmaLink> link;
    uint8_t* inbox = nullptr;
    ibv_mr* pool_mr = nullptr;
    ibv_mr* inbox_mr = nullptr;
    LinkHelloMsg remote;          // peer's registered regions
    bool hello_received = false;
    bool verified = false;
    uint64_t verify_write_ns = 0, verify_read_ns = 0;
    std::function<void(Status)> on_ready;

    ~PeerLink();
};

class LinkTable {
public:
    using MsgHandler = std::function<void(MachineId, Msg&&)>;
    using DownHandler = std::function<void(MachineId, Incarnation, const Status&)>;

    LinkTable(MachineId self, PagePool& pool, Metrics& metrics)
        : self_(self), pool_(pool), metrics_(metrics) {}

    void set_self(MachineId self) { self_ = self; }
    void set_handlers(MsgHandler on_msg, DownHandler on_down) {
        on_msg_ = std::move(on_msg);
        on_down_ = std::move(on_down);
    }

    // Takes ownership of an established link. `self_inc` is this node's own
    // incarnation (0 for the master). `ready` fires once LINK_HELLO has been
    // exchanged, or with an error.
    void add(MachineId peer, Incarnation peer_inc, Incarnation self_inc,
             std::unique_ptr<RdmaLink> link, std::function<void(Status)> ready);

    // Proves the data path: WRITE a random pattern into the peer's inbox,
    // READ it back, compare byte for byte.
    void verify(MachineId peer, std::function<void(Status)> done);

    PeerLink* get(MachineId peer);
    // Returns the link only if it is up and belongs to `inc`.
    PeerLink* get(MachineId peer, Incarnation inc);
    // Destroys the link: its queue pair, protection domain and every memory
    // registration made for that peer. No down handler is invoked.
    void drop(MachineId peer);
    void drop_all();
    void send(MachineId peer, const Msg& m);
    void send(MachineId peer, Msg&& m);
    size_t size() const { return peers_.size(); }
    const std::map<MachineId, std::unique_ptr<PeerLink>>& peers() const { return peers_; }

private:
    MachineId self_;
    PagePool& pool_;
    Metrics& metrics_;
    MsgHandler on_msg_;
    DownHandler on_down_;
    std::map<MachineId, std::unique_ptr<PeerLink>> peers_;
};

} // namespace dnuma
