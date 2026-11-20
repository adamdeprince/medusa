/* SPDX-License-Identifier: GPL-2.0-only */
// Core identifiers and enumerations shared by every dnuma component.
#pragma once

#include <cstdint>
#include <string>

namespace dnuma {

// A machine is one physical host participating in the system. Index 0 is
// always the master. A slave keeps the same MachineId for the lifetime of the
// master process: the id is bound to the slave's stable UUID, not its hostname.
using MachineId = uint32_t;
inline constexpr MachineId kMasterMachine = 0;
inline constexpr MachineId kNoMachine = UINT32_MAX;

// Persistent internal identity of one NUMA domain (machine UUID + physical
// node index). Never reused for a different domain while the master runs.
using NodeId = uint32_t;
inline constexpr NodeId kNoNode = UINT32_MAX;

// Every attachment of a slave gets a fresh incarnation. All state-changing
// messages carry it; anything from an older incarnation is rejected.
using Incarnation = uint64_t;

using ProcId = uint64_t;     // logical (distributed) process id
using CtxId = uint64_t;      // logical execution context (thread) id
using PageNo = uint64_t;     // virtual address >> 12
using Generation = uint64_t; // per-page ownership generation

inline constexpr uint64_t kPageSize = 4096; // coherence granularity (x86-64 base page)
inline constexpr unsigned kPageShift = 12;

// Default placement of the distributed virtual address range. It is the same
// in every image of a logical process, on every machine.
inline constexpr uint64_t kDefaultVaBase = 0x200000000000ull; // 32 TiB
inline constexpr uint64_t kDefaultVaSize = 0x010000000000ull; // 1 TiB reservation

// Tail of the distributed range that the master's allocator never hands out.
// Node daemons use it as scratch address space inside an image (for example
// to probe which remote memory-management operations the kernel permits).
inline constexpr uint64_t kReservedTailBytes = 8ull << 20;

enum class Access : uint8_t { None = 0, Read = 1, Write = 2 };

// Authoritative per-page state kept by the master's directory.
enum class PageState : uint8_t {
    Unbacked = 0,  // allocated, never touched: no physical copy anywhere
    Exclusive = 1, // exactly one machine holds the page and may write it
    Shared = 2,    // owner + readers hold identical read-only copies
    Lost = 3,      // authoritative copy was on a machine that failed
};

// Slave lifecycle (docs/lifecycle.md).
enum class SlaveState : uint8_t {
    Disconnected = 0,
    Connecting = 1,
    Active = 2,
    Draining = 3,
    Failed = 4,          // failure detected, fencing not yet started
    Fencing = 5,         // master-side isolation in progress
    FenceIncomplete = 6, // master side done; remote stop not yet proven
};

// How we know a detached slave has really stopped using its old incarnation.
enum class FenceProof : uint8_t {
    None = 0,
    AgentConfirmed = 1, // agent acknowledged destroying all contexts
    LeaseExpired = 2,   // agent's lease provably ran out (self-fence)
    CleanDetach = 3,    // graceful drain + detach handshake
    Reattached = 4,     // agent proved destruction during a later attach
};

// Revocation modes sent with PAGE_REVOKE.
enum class RevokeMode : uint8_t {
    ExportOnly = 0,       // keep read copy, stage a copy for a new reader
    DowngradeExport = 1,  // write -> read, stage a copy for a new reader
    InvalidateExport = 2, // drop all access, stage final contents
    Invalidate = 3,       // drop all access, contents not needed
};

// Where the data for a PAGE_GRANT comes from.
enum class GrantSource : uint8_t {
    Zero = 0,   // first touch: zero-fill locally
    Local = 1,  // machine already holds a valid copy (upgrade / re-grant)
    Remote = 2, // pull over RDMA from (machine, slot)
};

const char* to_string(Access a);
const char* to_string(PageState s);
const char* to_string(SlaveState s);
const char* to_string(FenceProof p);
const char* to_string(RevokeMode m);
const char* to_string(GrantSource s);

inline PageNo page_of(uint64_t addr) { return addr >> kPageShift; }
inline uint64_t addr_of(PageNo p) { return p << kPageShift; }

} // namespace dnuma
