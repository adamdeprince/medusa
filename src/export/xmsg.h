/* SPDX-License-Identifier: GPL-2.0-only */
// Messages for process export (docs/export.md). They travel on the
// operations channel between the master (C) and a slave's node (N).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "common/types.h"
#include "transport/wire.h"

namespace dnuma {

inline constexpr uint32_t kVmRead = 1, kVmWrite = 2, kVmExec = 4;

// Most pages one XPageReq may ask for. Bounds the size of the reply (1 MiB of
// page data), which is assembled in memory on both sides.
inline constexpr uint32_t kXMaxPagesPerRequest = 256;

struct XVma {
    uint64_t start = 0, end = 0;
    uint32_t prot = 0;
    template <class A> void io(A& a) { a(start, end, prot); }
};

struct XBlob { // contents of one pointer argument
    uint8_t arg = 0;
    std::vector<uint8_t> data;
    template <class A> void io(A& a) { a(arg, data); }
};

// The system-wide clocks a body reads without asking the master (see
// docs/export.md, "Time"). The table position is the index used on the wire.
inline constexpr clockid_t kXClockIds[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME, CLOCK_TAI};
inline constexpr size_t kXClocks = sizeof kXClockIds / sizeof kXClockIds[0];
using XClocks = std::array<uint64_t, kXClocks>; // nanoseconds, one per table entry

inline XClocks read_xclocks() {
    XClocks c{};
    for (size_t i = 0; i < kXClocks; ++i) {
        timespec ts{};
        clock_gettime(kXClockIds[i], &ts);
        c[i] = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    }
    return c;
}

// Table index for a clock id, or -1. The coarse variants are served from the
// precise clock: allowed, since "coarse" only permits lower resolution.
inline int xclock_index(int64_t id) {
    if (id == CLOCK_REALTIME_COARSE) id = CLOCK_REALTIME;
    if (id == CLOCK_MONOTONIC_COARSE) id = CLOCK_MONOTONIC;
    for (size_t i = 0; i < kXClocks; ++i)
        if (id == kXClockIds[i]) return int(i);
    return -1;
}

struct XClockMsg { // XClockReq (slave filled in) and XClock (both)
    XClocks slave{}, master{};
    template <class A> void io(A& a) { a(slave, master); }
};

struct XBeginMsg {
    uint64_t xid = 0;
    int32_t home_pid = 0;
    uint32_t uid = 0, gid = 0;
    std::vector<uint8_t> regs;   // struct user_regs_struct
    std::vector<uint8_t> xstate; // NT_X86_XSTATE regset
    std::vector<XVma> vmas;
    int32_t cpu = -1;            // CPU number this body is presented as on the master
    int32_t node = -1;           // logical NUMA node it is presented as running on
    int32_t local_cpu = -1;      // CPU of the slave to pin the body to (-1 = any)
    uint64_t rseq = 0;           // address of the process's restartable-sequences area (0 = none)
    XClocks clocks{};            // the master's clocks when the process was stopped: no
                                 // clock the body reads may show less
    template <class A> void io(A& a) { a(xid, home_pid, uid, gid, regs, xstate, vmas, cpu, node, local_cpu, rseq, clocks); }
};

struct XReadyMsg {
    uint64_t xid = 0;
    int32_t status = 0;
    std::string text;
    int32_t body_pid = 0;
    template <class A> void io(A& a) { a(xid, status, text, body_pid); }
};

struct XSyscallMsg {
    uint64_t xid = 0, seq = 0;
    int64_t nr = 0;
    std::array<uint64_t, 6> args{};
    std::vector<XBlob> in; // data the body is passing in
    template <class A> void io(A& a) { a(xid, seq, nr, args, in); }
};

struct XSysRetMsg {
    uint64_t xid = 0, seq = 0;
    int64_t ret = 0;
    std::vector<XBlob> out;     // data to store through the body's pointers
    std::vector<XVma> unmap;    // address-space changes to mirror, applied in
    std::vector<XVma> map;      //   this order: unmap, map, protect
    std::vector<XVma> protect;
    template <class A> void io(A& a) { a(xid, seq, ret, out, unmap, map, protect); }
};

struct XPageMsg { // XPageReq (addr, count) and XPage (addr, status, data)
    uint64_t xid = 0;
    uint64_t addr = 0;
    uint32_t count = 0;
    int32_t status = 0;
    std::vector<uint8_t> data; // count * 4096 bytes
    template <class A> void io(A& a) { a(xid, addr, count, status, data); }
};

// Page data is the bulk of what export moves, so XPage is also built and read
// in place, without the intermediate XPageMsg. The layout is the one io()
// above produces: xid, addr, count, status, data length, data.
inline constexpr size_t kXPageDataOffset = 8 + 8 + 4 + 4 + 4;

// A message with room for `pages` pages; fill the area at xpage_data() and
// call xpage_finish() with how many were actually provided.
inline Msg xpage_begin(uint64_t xid, uint64_t addr, uint32_t pages) {
    Msg m;
    m.type = MsgType::XPage;
    m.payload.resize(kXPageDataOffset + size_t(pages) * 4096);
    memcpy(m.payload.data(), &xid, 8);
    memcpy(m.payload.data() + 8, &addr, 8);
    return m;
}
inline uint8_t* xpage_data(Msg& m) { return m.payload.data() + kXPageDataOffset; }
inline void xpage_finish(Msg* m, uint32_t pages, int32_t status) {
    uint32_t len = pages * 4096;
    memcpy(m->payload.data() + 16, &pages, 4);
    memcpy(m->payload.data() + 20, &status, 4);
    memcpy(m->payload.data() + 24, &len, 4);
    m->payload.resize(kXPageDataOffset + len);
}

struct XPageView { // points into the message it was parsed from
    uint64_t xid = 0, addr = 0;
    uint32_t count = 0;
    int32_t status = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
};
inline bool parse_xpage(const Msg& m, XPageView* v) {
    if (m.payload.size() < kXPageDataOffset) return false;
    const uint8_t* p = m.payload.data();
    uint32_t len = 0;
    memcpy(&v->xid, p, 8);
    memcpy(&v->addr, p + 8, 8);
    memcpy(&v->count, p + 16, 4);
    memcpy(&v->status, p + 20, 4);
    memcpy(&len, p + 24, 4);
    if (m.payload.size() - kXPageDataOffset != len) return false;
    v->data = p + kXPageDataOffset;
    v->len = len;
    return true;
}

enum class XReason : uint32_t {
    None = 0,
    Requested = 1,   // the master asked (signal, drain, policy)
    Syscall = 2,     // a system call that must run natively at home
    Fault = 3,       // a synchronous fault to be re-raised at home
    Exit = 4,        // the process is exiting
    Error = 5,       // the node could not continue
};

struct XHomeMsg {
    uint64_t xid = 0;
    XReason reason{};
    int64_t nr = -1;        // system call number, for Syscall
    bool at_syscall = false; // regs are at a system-call entry stop (not yet executed)
    std::vector<uint8_t> regs, xstate;
    std::string text;
    uint64_t utime_ns = 0;  // CPU time the body consumed
    XClocks clock_last{};   // latest value of each clock the body was shown (0 = never read)
    template <class A> void io(A& a) { a(xid, reason, nr, at_syscall, regs, xstate, text, utime_ns, clock_last); }
};

struct XPagePushMsg {
    uint64_t xid = 0;
    std::vector<uint64_t> addrs;
    std::vector<uint8_t> data; // addrs.size() * 4096 bytes
    template <class A> void io(A& a) { a(xid, addrs, data); }
};

struct XSimpleMsg { // XStop, XKill, XHomeDone, XGone
    uint64_t xid = 0;
    int32_t status = 0;
    uint64_t count = 0;
    std::string text;
    template <class A> void io(A& a) { a(xid, status, count, text); }
};

} // namespace dnuma
