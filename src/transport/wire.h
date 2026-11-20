/* SPDX-License-Identifier: GPL-2.0-only */
// Wire protocol framing (docs/protocol.md).
//
// Every message on every channel (management TCP, RDMA SEND/RECV, local Unix
// sockets, in-process loopback) is one frame: a fixed 32-byte header followed
// by `payload_len` bytes. Authenticated stream channels append a 16-byte MAC.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "common/bytes.h"
#include "common/types.h"

namespace dnuma {

inline constexpr uint32_t kWireMagic = 0x414d4e44; // "DNMA" little-endian
inline constexpr uint16_t kProtoVersion = 1;
inline constexpr uint16_t kProtoVersionMin = 1;
inline constexpr size_t kHeaderSize = 32;
inline constexpr size_t kMacSize = 16;
// Protocol constraint: a frame larger than this is treated as corruption and
// the channel is closed. It bounds what a peer can make us buffer.
inline constexpr uint32_t kMaxPayload = 64u << 20;
inline constexpr uint32_t kMaxFdsPerFrame = 8;

enum class MsgType : uint16_t {
    // --- management channel (authenticated TCP, master <-> agent) ---
    Hello = 1,
    HelloAck = 2,
    Auth = 3,
    NodeAttach = 10,
    NodeAttachAck = 11,
    NodeAssign = 12,
    NodeAssignAck = 13,
    NodeDetach = 14,
    NodeDetachAck = 15,
    NodeDrain = 16,
    NodeFence = 17,
    NodeFenceAck = 18,
    ResourceReport = 20,
    Heartbeat = 21,    // agent -> master: lease renewal request
    HeartbeatAck = 22, // master -> agent: lease grant
    ViewSample = 23,   // agent -> master: CPU counters and hosted bodies, for the master's /proc view
    PeerExpect = 30,
    PeerConnect = 31,
    PeerFence = 32,
    PeerStatus = 33,
    FaultInject = 40,
    FaultInjectAck = 41,

    // --- operations channel (RDMA SEND/RECV, or loopback on the master) ---
    LinkHello = 50,
    LinkVerify = 51,
    LinkVerifyAck = 52,
    MemoryAllocate = 60,
    MemoryAllocateAck = 61,
    MemoryRelease = 62,
    PageRequest = 70,
    PageGrant = 71,
    PageRevoke = 72,
    PageRevoked = 73,
    PageTransferComplete = 74,
    PageRelease = 75,
    PageAbort = 76,
    ImageCreate = 80,
    ImageReady = 81,
    ImageExited = 82,
    ImageDestroy = 83,
    ExecCreate = 90,
    ExecStart = 91,
    ExecSuspend = 92,
    ExecMigrate = 93, // carries a suspended context's state to its new machine
    ExecTerminate = 94,
    ExecStarted = 95,
    ExecSuspended = 96,
    ExecExited = 97,
    SyscallRequest = 100,
    SyscallComplete = 101,
    AppCall = 110,
    AppReply = 111,

    // --- runtime channel (Unix socket, image <-> local node daemon) ---
    RtHello = 120,
    RtWelcome = 121,
    RtReady = 122,
    RtZap = 123,
    RtZapDone = 124,
    RtLease = 125,

    // --- process export (docs/export.md): operations channel ---
    XBegin = 140,    // C->N: create a body for a native process
    XReady = 141,    // N->C: body is running (or could not be created)
    XSyscall = 142,  // N->C: the body made a system call
    XSysRet = 143,   // C->N: result, executed by the process at home
    XPageReq = 144,  // N->C: the body touched memory it does not have
    XPage = 145,     // C->N: page contents from the process at home
    XStop = 146,     // C->N: stop the body and send its state home
    XHome = 147,     // N->C: body stopped: registers (and why, if the node decided)
    XPagePush = 148, // N->C: pages the body modified
    XHomeDone = 149, // N->C: all modified pages sent
    XKill = 150,     // C->N: destroy the body
    XGone = 151,     // N->C: the body died unexpectedly
    XClockReq = 152, // N->C: clock comparison request (the slave's readings)
    XClock = 153,    // C->N: the request's readings and the master's

    // --- control socket (dnumactl <-> master) ---
    CtlRequest = 130,
    CtlOutput = 131,
    CtlDone = 132,
};

const char* to_string(MsgType t);

struct Msg {
    MsgType type{};
    uint64_t req_id = 0;
    Incarnation inc = 0;
    std::vector<uint8_t> payload;
    // File descriptors travelling with the frame (Unix sockets only). The
    // receiver owns them and must close them.
    std::vector<int> fds;
};

struct WireHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_len;
    uint32_t nfds;
    uint64_t req_id;
    uint64_t incarnation;
};
static_assert(sizeof(WireHeader) == kHeaderSize, "wire header must be 32 bytes");

void encode_header(const Msg& m, uint8_t out[kHeaderSize]);
// Validates magic, version and length. Returns false on a malformed header.
bool decode_header(const uint8_t in[kHeaderSize], WireHeader* out, std::string* why);

// ---- symmetric field archives: one io() per struct drives both directions ----

class Enc {
public:
    explicit Enc(Writer& w) : w_(w) {}
    template <class... T> void operator()(const T&... v) { (put(v), ...); }
    static constexpr bool reading = false;

private:
    Writer& w_;
    void put(bool v) { w_.boolean(v); }
    void put(uint8_t v) { w_.u8(v); }
    void put(uint16_t v) { w_.u16(v); }
    void put(uint32_t v) { w_.u32(v); }
    void put(uint64_t v) { w_.u64(v); }
    void put(int32_t v) { w_.i32(v); }
    void put(int64_t v) { w_.i64(v); }
    void put(const std::string& v) { w_.str(v); }
    void put(const std::vector<uint8_t>& v) { w_.bytes(v); }
    template <size_t N> void put(const std::array<uint64_t, N>& v) {
        for (uint64_t x : v) w_.u64(x);
    }
    template <class T> void put(const std::vector<T>& v) {
        w_.u32(uint32_t(v.size()));
        for (const T& x : v) put(x);
    }
    template <class T> auto put(const T& v) -> std::enable_if_t<std::is_enum_v<T>> {
        put(static_cast<std::underlying_type_t<T>>(v));
    }
    template <class T> auto put(const T& v) -> decltype(const_cast<T&>(v).io(*this), void()) {
        const_cast<T&>(v).io(*this);
    }
    // Types with their own encode()/decode() pair (hardware reports, metrics).
    template <class T> auto put(const T& v) -> decltype(v.encode(w_), void()) { v.encode(w_); }
};

class Dec {
public:
    explicit Dec(Reader& r) : r_(r) {}
    template <class... T> void operator()(T&... v) { (get(v), ...); }
    static constexpr bool reading = true;

private:
    Reader& r_;
    void get(bool& v) { v = r_.boolean(); }
    void get(uint8_t& v) { v = r_.u8(); }
    void get(uint16_t& v) { v = r_.u16(); }
    void get(uint32_t& v) { v = r_.u32(); }
    void get(uint64_t& v) { v = r_.u64(); }
    void get(int32_t& v) { v = r_.i32(); }
    void get(int64_t& v) { v = r_.i64(); }
    void get(std::string& v) { v = r_.str(); }
    void get(std::vector<uint8_t>& v) { v = r_.bytes(); }
    template <size_t N> void get(std::array<uint64_t, N>& v) {
        for (uint64_t& x : v) x = r_.u64();
    }
    template <class T> void get(std::vector<T>& v) {
        uint32_t n = r_.count(1);
        v.clear();
        for (uint32_t i = 0; i < n && r_.ok(); ++i) {
            v.emplace_back();
            get(v.back());
        }
    }
    template <class T> auto get(T& v) -> std::enable_if_t<std::is_enum_v<T>> {
        std::underlying_type_t<T> u{};
        get(u);
        v = static_cast<T>(u);
    }
    template <class T> auto get(T& v) -> decltype(v.io(*this), void()) { v.io(*this); }
    template <class T> auto get(T& v) -> decltype(v.decode(r_), void()) {
        if (!v.decode(r_)) r_.raw(nullptr, SIZE_MAX); // latch the reader's error state
    }
};

template <class T>
Msg make_msg(MsgType type, const T& body, uint64_t req_id = 0, Incarnation inc = 0) {
    Writer w;
    Enc e(w);
    e(body);
    Msg m;
    m.type = type;
    m.req_id = req_id;
    m.inc = inc;
    m.payload = w.take();
    return m;
}

// Decodes a payload; fails unless the body parses and consumes every byte.
template <class T> bool parse_msg(const Msg& m, T* out) {
    Reader r(m.payload);
    Dec d(r);
    d(*out);
    return r.done();
}

template <class T> std::vector<uint8_t> encode_body(const T& body) {
    Writer w;
    Enc e(w);
    e(body);
    return w.take();
}
template <class T> bool decode_body(const std::vector<uint8_t>& bytes, T* out) {
    Reader r(bytes);
    Dec d(r);
    d(*out);
    return r.done();
}

} // namespace dnuma
