// SPDX-License-Identifier: GPL-2.0-only
#include "node/link_table.h"

#include <cstdlib>
#include <cstring>

#include "common/log.h"
#include "common/util.h"

namespace dnuma {

PeerLink::~PeerLink() {
    link.reset(); // deregisters everything registered through the link
    free(inbox);
}

void LinkTable::add(MachineId peer, Incarnation peer_inc, Incarnation self_inc,
                    std::unique_ptr<RdmaLink> link, std::function<void(Status)> ready) {
    drop(peer);
    auto pl = std::make_unique<PeerLink>();
    pl->machine = peer;
    pl->inc = peer_inc;
    pl->link = std::move(link);
    pl->on_ready = std::move(ready);
    PeerLink* p = pl.get();
    peers_[peer] = std::move(pl);

    auto fail = [this, peer](Status st) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        auto cb = std::move(it->second->on_ready);
        peers_.erase(it);
        if (cb) cb(st);
    };

    void* mem = nullptr;
    if (posix_memalign(&mem, 4096, kInboxBytes) != 0) return fail(Status::err(ENOMEM, "inbox"));
    p->inbox = static_cast<uint8_t*>(mem);
    memset(p->inbox, 0, kInboxBytes);

    // Pool: the peer may only read it. Inbox: the one place it may write.
    Status st = p->link->reg(pool_.base(), pool_.bytes(), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ,
                             &p->pool_mr);
    if (st.is_ok())
        st = p->link->reg(p->inbox, kInboxBytes,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE,
                          &p->inbox_mr);
    if (!st.is_ok()) return fail(st);

    p->link->start(
        [this, peer](Msg&& m) {
            auto it = peers_.find(peer);
            if (it == peers_.end()) return;
            PeerLink* pl = it->second.get();
            if (m.type == MsgType::LinkHello) {
                LinkHelloMsg h;
                if (!parse_msg(m, &h) || h.machine != peer || h.incarnation != pl->inc) {
                    LOG_ERROR("link[%u]: invalid LINK_HELLO (machine %u incarnation %llu)", peer, h.machine,
                              (unsigned long long)h.incarnation);
                    auto cb = std::move(pl->on_ready);
                    Incarnation inc = pl->inc;
                    // Keep the link alive until this handler (which the link
                    // itself is running) has returned.
                    std::shared_ptr<PeerLink> keep(it->second.release());
                    peers_.erase(it);
                    if (cb) cb(Status::err(EPROTO, "invalid LINK_HELLO from machine %u", peer));
                    else if (on_down_) on_down_(peer, inc, Status::err(EPROTO, "invalid LINK_HELLO"));
                    return;
                }
                pl->remote = h;
                pl->hello_received = true;
                auto cb = std::move(pl->on_ready);
                pl->on_ready = nullptr;
                if (cb) cb(Status::ok());
                return;
            }
            if (on_msg_) on_msg_(peer, std::move(m));
        },
        [this, peer](const Status& st) {
            auto it = peers_.find(peer);
            if (it == peers_.end()) return;
            Incarnation inc = it->second->inc;
            auto cb = std::move(it->second->on_ready);
            // The link object is destroyed after this handler returns.
            std::shared_ptr<PeerLink> keep(it->second.release());
            peers_.erase(it);
            if (cb) cb(st);
            else if (on_down_) on_down_(peer, inc, st);
        });

    LinkHelloMsg hello;
    hello.machine = self_;
    hello.incarnation = self_inc;
    hello.pool_base = reinterpret_cast<uint64_t>(pool_.base());
    hello.pool_len = pool_.bytes();
    hello.pool_rkey = p->pool_mr->rkey;
    hello.inbox_base = reinterpret_cast<uint64_t>(p->inbox);
    hello.inbox_len = kInboxBytes;
    hello.inbox_rkey = p->inbox_mr->rkey;
    p->link->send(make_msg(MsgType::LinkHello, hello));
}

void LinkTable::verify(MachineId peer, std::function<void(Status)> done) {
    PeerLink* p = get(peer);
    if (!p || !p->hello_received) return done(Status::err(ENOTCONN, "no link to machine %u", peer));
    if (p->remote.inbox_len < kVerifyBytes) return done(Status::err(EPROTO, "peer inbox too small"));
    // Local layout of our own inbox: [0,16K) is where the peer's self-test
    // writes; we use [32K,48K) as the pattern and [48K,64K) for read-back.
    uint8_t* pattern = p->inbox + 32 * 1024;
    uint8_t* readback = p->inbox + 48 * 1024;
    if (!random_bytes(pattern, kVerifyBytes).is_ok()) return done(Status::err(EIO, "no entropy"));
    memset(readback, 0, kVerifyBytes);
    uint64_t raddr = p->remote.inbox_base;
    uint32_t rkey = p->remote.inbox_rkey, lkey = p->inbox_mr->lkey;
    Incarnation inc = p->inc;
    p->link->write(pattern, lkey, raddr, rkey, uint32_t(kVerifyBytes),
                   [this, peer, inc, raddr, rkey, lkey, done](Status st, uint64_t wlat) {
        PeerLink* p = get(peer, inc);
        if (!st.is_ok() || !p) return done(st.is_ok() ? Status::err(ENOTCONN, "link lost") : st);
        p->verify_write_ns = wlat;
        metrics_.observe("rdma_write_ns", wlat);
        p->link->read(p->inbox + 48 * 1024, lkey, raddr, rkey, uint32_t(kVerifyBytes),
                      [this, peer, inc, done](Status st, uint64_t rlat) {
            PeerLink* p = get(peer, inc);
            if (!st.is_ok() || !p) return done(st.is_ok() ? Status::err(ENOTCONN, "link lost") : st);
            p->verify_read_ns = rlat;
            metrics_.observe("rdma_read_ns", rlat);
            if (memcmp(p->inbox + 32 * 1024, p->inbox + 48 * 1024, kVerifyBytes) != 0)
                return done(Status::err(EIO, "RDMA self-test: data read back differs from data written"));
            p->verified = true;
            done(Status::ok());
        });
    });
}

PeerLink* LinkTable::get(MachineId peer) {
    auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : it->second.get();
}

PeerLink* LinkTable::get(MachineId peer, Incarnation inc) {
    PeerLink* p = get(peer);
    if (!p || p->inc != inc || !p->link || !p->link->is_up() || !p->hello_received) return nullptr;
    return p;
}

void LinkTable::drop(MachineId peer) { peers_.erase(peer); }
void LinkTable::drop_all() { peers_.clear(); }

void LinkTable::send(MachineId peer, const Msg& m) {
    PeerLink* p = get(peer);
    if (p && p->link) p->link->send(m);
}

void LinkTable::send(MachineId peer, Msg&& m) {
    PeerLink* p = get(peer);
    if (p && p->link) p->link->send(std::move(m));
}

} // namespace dnuma
