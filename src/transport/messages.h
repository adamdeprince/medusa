/* SPDX-License-Identifier: GPL-2.0-only */
// Typed message bodies. Each struct lists its fields once in io(); the same
// list drives encoding and decoding, so the two can never disagree.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/hwprobe.h"
#include "common/metrics.h"
#include "common/types.h"
#include "transport/wire.h"

namespace dnuma {

// ---------------- management channel ----------------

struct HelloMsg {
    uint16_t ver_min = kProtoVersionMin, ver_max = kProtoVersion;
    uint64_t master_instance = 0;
    std::vector<uint8_t> nonce; // 32 random bytes
    template <class A> void io(A& a) { a(ver_min, ver_max, master_instance, nonce); }
};

struct HelloAckMsg {
    int32_t status = 0;
    std::string error;
    uint16_t version = 0;
    uint64_t features = 0;
    std::string uuid;     // stable slave identity (distinct from hostname)
    std::string hostname;
    std::vector<uint8_t> nonce;
    std::vector<uint8_t> mac; // HMAC(cluster key, "agent" | nonce_m | nonce_a | uuid)
    template <class A> void io(A& a) { a(status, error, version, features, uuid, hostname, nonce, mac); }
};

struct AuthMsg {
    std::vector<uint8_t> mac; // HMAC(cluster key, "master" | nonce_a | nonce_m | instance)
    template <class A> void io(A& a) { a(mac); }
};

struct NodeAttachMsg {
    MachineId machine = 0;
    Incarnation incarnation = 0;
    uint64_t master_instance = 0;
    uint32_t lease_ms = 0;
    uint32_t renew_ms = 0;
    std::string master_hostname;
    uint32_t view_ms = 0; // send a VIEW_SAMPLE this often (0 = the master has no use for them)
    template <class A> void io(A& a) {
        a(machine, incarnation, master_instance, lease_ms, renew_ms, master_hostname, view_ms);
    }
};

struct NodeAttachAckMsg {
    int32_t status = 0;
    std::string error;
    uint32_t stale_images_destroyed = 0;   // leftovers from an earlier incarnation
    uint32_t stale_contexts_destroyed = 0;
    MachineResources resources;
    std::string rdma_ip;
    uint16_t rdma_port = 0;
    uint64_t pool_bytes = 0;      // registered page pool
    uint64_t capacity_bytes = 0;  // memory this machine offers to distributed processes
    template <class A> void io(A& a) {
        a(status, error, stale_images_destroyed, stale_contexts_destroyed, resources, rdma_ip,
          rdma_port, pool_bytes, capacity_bytes);
    }
};

struct NodeAssignEntry {
    int32_t phys_index = 0;
    NodeId node = 0;
    int32_t logical = 0;
    template <class A> void io(A& a) { a(phys_index, node, logical); }
};
struct NodeAssignMsg {
    std::vector<NodeAssignEntry> nodes;
    template <class A> void io(A& a) { a(nodes); }
};

// Generic acknowledgement / small notification.
struct SimpleMsg {
    int32_t status = 0;
    std::string text;
    uint64_t a = 0, b = 0;
    template <class A> void io(A& ar) { ar(status, text, a, b); }
};

struct HeartbeatMsg {
    uint64_t seq = 0;
    template <class A> void io(A& a) { a(seq); }
};
struct HeartbeatAckMsg {
    uint64_t seq = 0;
    uint32_t lease_ms = 0; // 0 = lease refused (node is being fenced)
    template <class A> void io(A& a) { a(seq, lease_ms); }
};

struct ResourceReportMsg {
    std::vector<uint64_t> node_mem_free;     // per physical node, bytes
    std::vector<uint64_t> node_resident;     // distributed pages resident per physical node
    uint64_t pool_slots_total = 0, pool_slots_used = 0;
    uint32_t images = 0, contexts = 0;
    uint32_t bodies = 0;   // exported processes executing here
    uint32_t runnable = 0; // kernel run-queue length right now
    Metrics metrics;
    template <class A> void io(A& a) {
        a(node_mem_free, node_resident, pool_slots_total, pool_slots_used, images, contexts, bodies, runnable, metrics);
    }
};

// What the slave's own kernel says about the CPUs it offers and about the
// bodies of exported processes, sampled every NodeAttachMsg::view_ms. The
// master shows it in its own /proc (see master/kernel_view.h).
struct ViewSampleMsg {
    uint32_t clk_tck = 0;            // unit of all times below, per second
    std::vector<int32_t> cpus;       // CPU numbers on the slave
    std::vector<uint64_t> cpu_times; // for each of them, the 10 counters of its /proc/stat line
    // For each body: export id, state character from /proc/<pid>/stat, user
    // time, system time.
    std::vector<uint64_t> bodies;
    template <class A> void io(A& a) { a(clk_tck, cpus, cpu_times, bodies); }
};

struct PeerMsg {
    MachineId peer = 0;
    Incarnation peer_inc = 0;
    std::string rdma_ip;
    uint16_t rdma_port = 0;
    int32_t status = 0;
    std::string text;
    template <class A> void io(A& a) { a(peer, peer_inc, rdma_ip, rdma_port, status, text); }
};

struct FaultInjectMsg {
    std::string what;
    uint64_t arg = 0;
    template <class A> void io(A& a) { a(what, arg); }
};

// ---------------- operations channel ----------------

struct LinkHelloMsg {
    MachineId machine = 0;
    Incarnation incarnation = 0;
    uint64_t pool_base = 0, pool_len = 0;
    uint32_t pool_rkey = 0;
    uint64_t inbox_base = 0, inbox_len = 0;
    uint32_t inbox_rkey = 0;
    template <class A> void io(A& a) {
        a(machine, incarnation, pool_base, pool_len, pool_rkey, inbox_base, inbox_len, inbox_rkey);
    }
};

inline constexpr uint8_t kPageFlagPark = 1;         // grant: keep the page parked, do not map
inline constexpr uint8_t kPageFlagRestoreWrite = 2; // abort: source regains write access
inline constexpr uint8_t kPageFlagHandoff = 4;      // grant(local): ownership handed over

// One body for every PAGE_* message; unused fields are zero.
struct PageMsg {
    ProcId proc = 0;
    PageNo page = 0;
    Generation gen = 0;
    Access access = Access::None;
    RevokeMode mode = RevokeMode::Invalidate;
    GrantSource source = GrantSource::Zero;
    uint8_t flags = 0;
    int32_t status = 0;
    MachineId src_machine = kNoMachine;
    Incarnation src_inc = 0;
    uint32_t slot = 0;
    NodeId home_node = kNoNode;
    template <class A> void io(A& a) {
        a(proc, page, gen, access, mode, source, flags, status, src_machine, src_inc, slot, home_node);
    }
};

struct MemoryMsg {
    ProcId proc = 0;
    PageNo first = 0;
    uint64_t count = 0; // 0 with first == 0 means "everything for this process"
    NodeId node = kNoNode;
    Generation gen = 0;
    int32_t status = 0;
    template <class A> void io(A& a) { a(proc, first, count, node, gen, status); }
};

inline constexpr uint8_t kImageKernelFaults = 1; // uffd handles kernel-mode faults
inline constexpr uint8_t kImageRemoteZap = 2;    // pager can unmap without the image's help

struct ImageMsg {
    ProcId proc = 0;
    std::string exe;
    std::vector<uint8_t> exe_hash;
    std::vector<std::string> argv;
    uint64_t va_base = 0, va_size = 0;
    uint64_t canary = 0;
    uint32_t uid = 0, gid = 0;
    int32_t pid = 0;
    int32_t status = 0;
    std::string text;
    uint64_t uffd_features = 0;
    uint8_t flags = 0;
    template <class A> void io(A& a) {
        a(proc, exe, exe_hash, argv, va_base, va_size, canary, uid, gid, pid, status, text,
          uffd_features, flags);
    }
};

inline constexpr uint32_t kExecCancelled = 1;

struct ExecMsg {
    ProcId proc = 0;
    CtxId ctx = 0;
    uint64_t entry = 0, arg = 0;
    uint64_t stack_base = 0, stack_size = 0;
    NodeId node = kNoNode;
    uint64_t sp = 0;     // saved stack pointer of a suspended context
    uint64_t result = 0;
    int32_t tid = 0, cpu = -1, phys_node = -1;
    int32_t status = 0;
    uint32_t flags = 0;
    std::string host;
    std::vector<int32_t> cpus; // CPUs the carrier thread is pinned to
    template <class A> void io(A& a) {
        a(proc, ctx, entry, arg, stack_base, stack_size, node, sp, result, tid, cpu, phys_node,
          status, flags, host, cpus);
    }
};

struct SyscallMsg {
    ProcId proc = 0;
    CtxId ctx = 0;
    uint64_t call = 0;
    MachineId origin = 0;
    int64_t nr = 0;
    std::array<uint64_t, 6> args{};
    int64_t ret = 0;
    int32_t err = 0;
    template <class A> void io(A& a) { a(proc, ctx, call, origin, nr, args, ret, err); }
};

enum class AppOp : uint32_t {
    Alloc = 1,
    Free = 2,
    ThreadCreate = 3,
    ThreadJoin = 4,
    Topology = 5,
    MigratePages = 6,
    PageInfo = 7,
    ThreadMigrate = 8,
};

struct AppCallMsg {
    ProcId proc = 0;
    MachineId origin = 0;
    uint64_t call = 0;
    AppOp op{};
    int32_t status = 0; // replies only
    std::string error;  // replies only
    std::vector<uint8_t> body;
    template <class A> void io(A& a) { a(proc, origin, call, op, status, error, body); }
};

// AppCall bodies.
struct AllocReq {
    uint64_t bytes = 0;
    int32_t logical_node = -1;
    uint32_t flags = 0;
    template <class A> void io(A& a) { a(bytes, logical_node, flags); }
};
struct AddrMsg {
    uint64_t addr = 0, len = 0;
    int32_t logical_node = -1;
    uint64_t a0 = 0, a1 = 0;
    template <class A> void io(A& a) { a(addr, len, logical_node, a0, a1); }
};
struct ThreadCreateReq {
    int32_t logical_node = -1;
    uint64_t entry = 0, arg = 0, stack_size = 0;
    uint32_t flags = 0;
    template <class A> void io(A& a) { a(logical_node, entry, arg, stack_size, flags); }
};
struct ThreadInfo {
    CtxId ctx = 0;
    uint64_t result = 0;
    std::string host;
    int32_t cpu = -1, tid = 0, logical_node = -1, phys_node = -1;
    uint32_t migrations = 0, flags = 0;
    template <class A> void io(A& a) {
        a(ctx, result, host, cpu, tid, logical_node, phys_node, migrations, flags);
    }
};
struct TopoNode {
    int32_t logical = 0;
    NodeId node = 0;
    MachineId machine = 0;
    std::string host;
    int32_t phys_index = 0;
    uint32_t ncpus = 0;
    uint64_t mem_total = 0, mem_free = 0;
    SlaveState state{};
    bool is_master = false;
    template <class A> void io(A& a) {
        a(logical, node, machine, host, phys_index, ncpus, mem_total, mem_free, state, is_master);
    }
};
struct TopologyReply {
    std::vector<TopoNode> nodes;
    template <class A> void io(A& a) { a(nodes); }
};
struct PageInfoReply {
    PageState state{};
    MachineId owner = kNoMachine;
    std::string owner_host;
    Generation gen = 0;
    std::vector<MachineId> readers;
    int32_t home_logical = -1;
    template <class A> void io(A& a) { a(state, owner, owner_host, gen, readers, home_logical); }
};

// ---------------- runtime channel ----------------

enum class ImageRole : uint8_t { Home = 0, Shadow = 1 };

// RT_HELLO/RT_WELCOME request ID for a shadow willing to map the agent's
// private shmem backing file. A zero reply uses the original anonymous map.
inline constexpr uint64_t kRtShmemBacking = 1;

struct RtHelloMsg {
    ImageRole role{};
    int32_t pid = 0;
    std::string exe;
    std::vector<std::string> argv;
    uint64_t canary = 0;
    template <class A> void io(A& a) { a(role, pid, exe, argv, canary); }
};
struct RtWelcomeMsg {
    int32_t status = 0;
    std::string text;
    ProcId proc = 0;
    MachineId machine = 0;
    std::string hostname;
    uint64_t va_base = 0, va_size = 0;
    uint64_t canary = 0;
    uint32_t uid = 0, gid = 0;
    template <class A> void io(A& a) {
        a(status, text, proc, machine, hostname, va_base, va_size, canary, uid, gid);
    }
};
struct RtReadyMsg {
    int32_t status = 0;
    std::string text;
    uint64_t uffd_features = 0;
    bool kernel_faults = false;
    template <class A> void io(A& a) { a(status, text, uffd_features, kernel_faults); }
};
struct RtZapMsg {
    uint64_t id = 0, addr = 0, len = 0;
    int32_t status = 0;
    template <class A> void io(A& a) { a(id, addr, len, status); }
};

// ---------------- control socket ----------------

struct CtlRequestMsg {
    std::vector<std::string> args;
    template <class A> void io(A& a) { a(args); }
};
struct CtlOutputMsg {
    std::string text;
    template <class A> void io(A& a) { a(text); }
};
struct CtlDoneMsg {
    int32_t code = 0;
    template <class A> void io(A& a) { a(code); }
};

} // namespace dnuma
