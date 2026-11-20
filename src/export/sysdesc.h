/* SPDX-License-Identifier: GPL-2.0-only */
// System-call descriptors for process export.
//
// When an exported process makes a system call on a slave, the call is
// executed by the original process on the master. Its pointer arguments
// refer to memory that lives with the body on the slave, so each call needs a
// description of what its pointers point at: what to carry to the master
// before the call and what to carry back afterwards. A call that has no
// descriptor here is not guessed at: the process is brought home and the
// call runs natively.
//
// The table is for x86-64. Sizes are those of the kernel ABI structures.
#pragma once

#include <cstdint>

namespace dnuma {

enum class SysClass : uint8_t {
    Forward, // execute at home with marshalled arguments
    Memory,  // changes the address space: execute at home, mirror on the slave
    Local,   // answered on the slave (per-thread CPU state)
    Home,    // bring the process home and run it natively
    Exit,    // process exit
};

enum class ArgKind : uint8_t {
    Val,     // plain value (integer, descriptor, or an address used as a value)
    Str,     // in: NUL-terminated string
    In,      // in: buffer, length in argument `len`
    Out,     // out: buffer, capacity in argument `len`, valid bytes = return value
    OutAll,  // out: buffer, capacity in argument `len`, entirely valid on success
    SIn,     // in: structure of `size` bytes
    SOut,    // out: structure of `size` bytes
    SInOut,  // in and out: structure of `size` bytes
    IovIn,   // in: struct iovec array, count in argument `len`
    IovOut,  // out: struct iovec array, count in argument `len`
    PollFds, // in and out: struct pollfd array, count in argument `len`
};

struct ArgDesc {
    ArgKind kind = ArgKind::Val;
    uint8_t len = 0;     // index of the argument holding a length / count
    uint32_t size = 0;   // fixed size for structure kinds
};

struct SysDesc {
    int nr = -1;
    const char* name = "";
    SysClass cls = SysClass::Home;
    ArgDesc a[6];
};

// Returns the descriptor for a call, taking request-dependent calls (ioctl,
// fcntl, futex, prctl, madvise) into account. Never returns null: unknown
// calls yield a descriptor of class Home.
const SysDesc& describe_syscall(int64_t nr, const uint64_t args[6]);

inline bool is_pointer(ArgKind k) { return k != ArgKind::Val; }
inline bool carries_in(ArgKind k) {
    return k == ArgKind::Str || k == ArgKind::In || k == ArgKind::SIn || k == ArgKind::SInOut ||
           k == ArgKind::IovIn || k == ArgKind::PollFds;
}
inline bool carries_out(ArgKind k) {
    return k == ArgKind::Out || k == ArgKind::OutAll || k == ArgKind::SOut || k == ArgKind::SInOut ||
           k == ArgKind::IovOut || k == ArgKind::PollFds;
}

} // namespace dnuma
