/* SPDX-License-Identifier: GPL-2.0-only */
// Framed message channels over stream sockets (TCP and Unix).
//
//  * StreamChannel: non-blocking, reactor-driven; used by the daemons.
//  * send_msg_blocking / recv_msg_blocking: used by the runtime library and
//    dnumactl, which are ordinary blocking clients.
//
// TCP management channels are authenticated: after the HELLO/AUTH handshake
// every frame carries a MAC over (sequence number | header | payload), so a
// frame cannot be forged, replayed or reordered. Unix channels rely on
// filesystem permissions and SO_PEERCRED, and can pass file descriptors.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/reactor.h"
#include "common/status.h"
#include "common/util.h"
#include "transport/wire.h"

namespace dnuma {

class StreamChannel {
public:
    using MsgHandler = std::function<void(Msg&&)>;
    using CloseHandler = std::function<void(const Status&)>;

    StreamChannel(Reactor& reactor, Fd fd, bool pass_fds);
    ~StreamChannel();
    StreamChannel(const StreamChannel&) = delete;
    StreamChannel& operator=(const StreamChannel&) = delete;

    // The close handler runs at most once, when the peer closes or an error
    // occurs. It may destroy the channel.
    void start(MsgHandler on_msg, CloseHandler on_close);
    void send(Msg m);
    // Local close: no close handler is invoked.
    void close();
    bool is_open() const { return fd_.valid(); }
    int fd() const { return fd_.get(); }

    // Turns on per-frame MACs in both directions for all later frames.
    void enable_mac(std::vector<uint8_t> key);

    // Bytes accepted by send() that the kernel has not taken yet. A producer
    // can stop when this grows and resume from the drain handler, which runs
    // each time the queue empties.
    size_t queued_bytes() const { return wq_bytes_; }
    void set_drain_handler(std::function<void()> fn) { on_drain_ = std::move(fn); }
    // Closes locally once everything queued has been written.
    void close_when_flushed() {
        closing_ = true;
        if (wq_.empty()) close();
    }

    uint64_t bytes_sent() const { return tx_bytes_; }
    uint64_t bytes_received() const { return rx_bytes_; }

private:
    struct OutBuf {
        std::vector<uint8_t> data;
        size_t off = 0;
        std::vector<int> fds;
    };
    void on_events(uint32_t ev);
    bool do_read(Status* err);
    bool do_write(Status* err);
    bool parse(Status* err, const std::shared_ptr<bool>& alive);
    void fail(const Status& st);
    void update_interest();

    Reactor& reactor_;
    Fd fd_;
    bool pass_fds_;
    MsgHandler on_msg_;
    CloseHandler on_close_;
    std::vector<uint8_t> rbuf_;
    size_t rpos_ = 0;
    std::deque<int> rfds_;
    std::deque<OutBuf> wq_;
    bool want_write_ = false, closing_ = false;
    size_t wq_bytes_ = 0;
    std::function<void()> on_drain_;
    bool mac_ = false;
    std::vector<uint8_t> mac_key_;
    uint64_t tx_seq_ = 0, rx_seq_ = 0;
    uint64_t tx_bytes_ = 0, rx_bytes_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

Status send_msg_blocking(int fd, const Msg& m);
// Returns ECONNRESET-coded status on clean EOF.
Status recv_msg_blocking(int fd, Msg* m);

Status tcp_listen(const std::string& bind_addr, uint16_t port, Fd* out, uint16_t* bound_port);
Status unix_listen(const std::string& path, int mode, Fd* out);
Status unix_connect(const std::string& path, Fd* out);
// Asynchronous TCP connect with a deadline. Name resolution is synchronous
// (getaddrinfo); the connect itself is event-driven.
void tcp_connect_async(Reactor& reactor, const std::string& host, uint16_t port, uint64_t timeout_ms,
                       std::function<void(Status, Fd, std::string peer_ip)> done);
// Accepts one pending connection (non-blocking listener). Returns an invalid
// Fd when nothing is pending.
Fd accept_one(int listen_fd, std::string* peer);
void close_fds(std::vector<int>& fds);

} // namespace dnuma
