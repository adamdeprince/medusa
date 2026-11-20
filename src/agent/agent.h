/* SPDX-License-Identifier: GPL-2.0-only */
// dnuma-agent: the slave-side service.
//
// The agent contributes this machine's CPUs and memory to a master. It owns
// no global decisions: page ownership, placement and process identity all
// come from the master. What it does own is local enforcement - most
// importantly *fencing*: whenever its attachment ends for any reason
// (explicit fence, detach, lost connection, lost RDMA link, lease expiry, a
// newer attach), it destroys every image and context of that incarnation
// before doing anything else.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/hwprobe.h"
#include "common/metrics.h"
#include "agent/body.h"
#include "common/reactor.h"
#include "node/link_table.h"
#include "node/node_core.h"
#include "node/page_pool.h"
#include "transport/messages.h"
#include "transport/rdma.h"
#include "transport/stream.h"

namespace dnuma {

struct AgentConfig {
    std::string listen_addr;      // management listener (empty = all addresses)
    uint16_t port = 7710;
    std::string rdma_ip;          // empty = first ACTIVE InfiniBand port with an address
    uint16_t rdma_port = 7711;
    std::string state_dir = "/var/lib/dnuma";
    std::string body_stub; // path of dnuma-body (default: next to this executable)
    unsigned body_workers = 0; // threads hosting bodies (0 = one per 6 CPUs)
    // NUMA nodes (kernel numbers) offered to the cluster. Empty = the whole
    // machine. Otherwise the agent and everything it hosts are confined to
    // these nodes' CPUs and memory.
    std::vector<int> nodes;
    std::string key_file = "/etc/dnuma/cluster.key";
    uint64_t pool_bytes = 256ull << 20;
    uint64_t capacity_bytes = 0;  // 0 = half of MemAvailable at start
};

class Agent {
public:
    Agent(Reactor& reactor, AgentConfig cfg);
    ~Agent();
    Status init();
    void shutdown();
    void on_sigchld() {
        if (bodies_) bodies_->on_sigchld();
    }

private:
    struct Conn {
        std::unique_ptr<StreamChannel> chan;
        std::string peer;
        std::vector<uint8_t> nonce_m, nonce_a;
        uint64_t master_instance = 0;
        bool hello = false, authed = false;
    };

    void on_accept();
    void on_conn_msg(Conn* c, Msg&& m);
    void on_conn_closed(Conn* c, const Status& st);
    void close_conn(Conn* c);
    void on_hello(Conn* c, const Msg& m);
    void on_auth(Conn* c, const Msg& m);
    void on_attach(Conn* c, const Msg& m);
    void on_fault(Conn* c, const Msg& m);
    void send_mgmt(MsgType type, const Msg& m);
    template <class T> void reply(Conn* c, MsgType type, const T& body) {
        if (c->chan && c->chan->is_open()) c->chan->send(make_msg(type, body, 0, inc_));
    }

    // Ends the current incarnation: destroys all images, contexts and pages.
    void self_fence(const std::string& why, uint32_t* images = nullptr, uint32_t* contexts = nullptr,
                    bool keep_links = false);
    void send_heartbeat();
    void send_report();
    void send_view_sample();
    void arm_lease(uint64_t expiry_ns);
    void on_ops_msg(MachineId from, Msg&& m);
    bool on_rdma_request(const RdmaEnv::Incoming& in, std::vector<uint8_t>* reply, RdmaEnv::LinkReady* ready);
    void peer_connect(const PeerMsg& p);
    void stale_access_probe();

    Reactor& reactor_;
    AgentConfig cfg_;
    std::vector<uint8_t> key_;
    std::string uuid_;
    MachineResources res_;
    std::string rdma_ip_;
    uint16_t rdma_port_ = 0;
    uint64_t capacity_bytes_ = 0;

    Fd listen_;
    std::map<Conn*, std::unique_ptr<Conn>> conns_;
    Conn* master_ = nullptr; // the connection that owns the current incarnation

    Metrics metrics_;
    RdmaEnv rdma_;
    PagePool pool_;
    LinkTable links_;
    std::unique_ptr<NodeCore> node_;
    std::unique_ptr<BodyShards> bodies_; // exported native processes executing here

    // Current incarnation (inc_ == 0 means "not attached").
    MachineId machine_ = kNoMachine;
    Incarnation inc_ = 0;
    uint64_t master_instance_ = 0;
    uint32_t lease_ms_ = 0, renew_ms_ = 0;
    uint64_t lease_expiry_ns_ = 0;
    uint64_t hb_seq_ = 0;
    std::map<uint64_t, uint64_t> hb_sent_; // seq -> local send time
    Reactor::TimerId hb_timer_ = 0, lease_timer_ = 0, report_timer_ = 0, view_timer_ = 0;
    std::map<MachineId, Incarnation> expected_peers_;
    uint64_t fences_ = 0;
    Incarnation last_fenced_inc_ = 0; // what the most recent self-fence destroyed
    uint32_t last_fenced_images_ = 0, last_fenced_contexts_ = 0;

    // Fault injection.
    bool partitioned_ = false;
};

} // namespace dnuma
