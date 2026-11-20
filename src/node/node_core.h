/* SPDX-License-Identifier: GPL-2.0-only */
// NodeCore: the per-machine memory manager and execution host.
//
// One NodeCore runs inside each daemon (the master hosts the one for machine
// 0; every agent hosts one for its slave). It plays the role a kernel would
// play for a distributed process on that machine:
//
//   * owns the userfaultfd of every local image and resolves its faults;
//   * holds the machine's parked pages in the registered page pool;
//   * applies ownership grants and revocations decided by the master;
//   * pulls page contents from peers with RDMA READ;
//   * creates images and relays execution-context control.
//
// It never decides page ownership itself: every transition is driven by a
// message from the master's coordinator (docs/memory.md).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/hwprobe.h"
#include "common/metrics.h"
#include "common/reactor.h"
#include "node/link_table.h"
#include "node/page_pool.h"
#include "transport/messages.h"
#include "transport/stream.h"

namespace dnuma {

struct NodeConfig {
    MachineId self = kMasterMachine;
    bool is_master = false;
    std::string hostname;
    uint64_t zap_timeout_ms = 10000;
    uint64_t image_start_timeout_ms = 20000;
    // Fault injection: stall every RDMA page pull by this long before issuing it.
    uint64_t test_pull_delay_ms = 0;
};

class NodeCore {
public:
    struct Hooks {
        // Sends an operations-channel message to the master's coordinator.
        std::function<void(Msg)> to_coordinator;
        // Master only: registers a new logical process for a home image.
        std::function<ProcId(const RtHelloMsg&, int pid, uint32_t uid, uint32_t gid, uint64_t* va_base,
                             uint64_t* va_size, std::string* err)>
            register_home;
    };

    NodeCore(Reactor& reactor, NodeConfig cfg, PagePool& pool, LinkTable& links, Metrics& metrics,
             Hooks hooks);
    ~NodeCore();

    void set_resources(const MachineResources& res) { res_ = res; }
    void set_incarnation(Incarnation inc) { inc_ = inc; }
    void set_self(MachineId self) { cfg_.self = self; }
    // NodeId <-> physical NUMA index mapping assigned by the master.
    void set_node_map(const std::vector<NodeAssignEntry>& nodes);

    // Handles one operations-channel message from the coordinator.
    void handle(Msg&& m);
    // Master: a runtime connected on the application socket.
    void adopt_connection(Fd fd);

    // Destroys every image and drops every page (fence, detach, lease expiry).
    void reset(const char* why, uint32_t* images_destroyed, uint32_t* contexts_destroyed);

    uint32_t image_count() const;
    uint32_t context_count() const;
    std::vector<uint64_t> resident_by_node() const;
    void set_test_pull_delay(uint64_t ms) { cfg_.test_pull_delay_ms = ms; }

private:
    enum class Where : uint8_t { Absent = 0, Mapped = 1, Parked = 2 };

    struct LocalPage {
        Access right = Access::None;     // what the master has granted this machine
        Where where = Where::Absent;
        bool mapped_rw = false;
        bool zapping = false;            // cooperative unmap in flight
        int32_t slot = -1;               // parked copy
        int32_t export_slot = -1;        // staging copy for a peer (== slot when parked)
        Access requested = Access::None; // highest outstanding PAGE_REQUEST
        int16_t phys_node = -1;          // where the resident copy was placed
        uint64_t fault_t0 = 0;
    };

    struct Image {
        ProcId proc = 0;
        ImageRole role{};
        int pid = 0;
        Fd pidfd, uffd, pagemap;
        std::unique_ptr<StreamChannel> chan;
        bool ready = false, dead = false;
        uint64_t uffd_features = 0;
        bool kernel_faults = false;
        bool remote_zap = false; // kernel lets the pager unmap this image's pages itself
        std::unordered_map<uint64_t, std::function<void(Status)>> zaps;
        std::unordered_map<int, CtxId> tid_ctx;
        std::unordered_map<CtxId, int> ctx_phys;
        uint64_t create_req = 0; // req id of the IMAGE_CREATE being answered
        Reactor::TimerId start_timer = 0;
    };

    struct Proc {
        ProcId id = 0;
        uint64_t va_base = 0, va_size = 0;
        uint64_t canary = 0;
        uint32_t uid = 0, gid = 0;
        std::unique_ptr<Image> image;
        std::unordered_map<PageNo, LocalPage> pages;
    };

    Proc* find_proc(ProcId id);
    Proc& get_proc(ProcId id);
    void drop_proc(ProcId id, bool kill_image);
    int phys_of(NodeId node) const;
    std::vector<int32_t> cpus_of(int phys) const;

    // images
    void on_image_msg(Image* img, ProcId proc_hint, Msg&& m);
    void on_image_closed(ProcId proc, StreamChannel* chan);
    void on_rt_hello(std::unique_ptr<StreamChannel>& chan, Msg&& m, bool from_listener);
    void on_rt_ready(Proc& p, Msg&& m);
    void on_image_exit(ProcId proc);
    void spawn_shadow(const ImageMsg& im, uint64_t req);
    void kill_image(Image& img);
    void fail_image_create(ProcId proc, const std::string& why);

    // paging
    void on_uffd(ProcId proc);
    void on_fault(Proc& p, uint64_t addr, uint64_t flags, int tid);
    void on_grant(const PageMsg& g);
    void on_revoke(const PageMsg& r);
    void finish_revoke(ProcId proc, PageNo page, Generation gen, RevokeMode mode, int32_t export_slot,
                       uint64_t t0, Status st);
    void on_release(const PageMsg& r);
    void on_abort(const PageMsg& r);
    void on_memory_allocate(const MemoryMsg& mm, uint64_t req);
    void request_page(Proc& p, PageNo page, LocalPage& lp, Access need);
    void send_complete(const PageMsg& g, int status);
    Status install(Proc& p, PageNo page, LocalPage& lp, const uint8_t* src, bool rw, int phys);
    Status install_from_parked(Proc& p, PageNo page, LocalPage& lp);
    Status read_image_page(Image& img, uint64_t addr, uint8_t* dst);
    Status uffd_wp(Image& img, uint64_t addr, bool protect);
    void uffd_wake(Image& img, uint64_t addr);
    bool page_present(Image& img, uint64_t addr, bool* known);
    bool remote_zap(Image& img, uint64_t addr, uint64_t len);
    void zap(Image& img, uint64_t addr, uint64_t len, std::function<void(Status)> done);
    void segv(Image& img, int tid, uint64_t addr);
    void account(LocalPage& lp, int phys, bool add);
    void maybe_erase(Proc& p, PageNo page);
    void free_slot(int32_t slot);

    Reactor& reactor_;
    NodeConfig cfg_;
    PagePool& pool_;
    LinkTable& links_;
    Metrics& metrics_;
    Hooks hooks_;
    MachineResources res_;
    Incarnation inc_ = 0;
    std::unordered_map<NodeId, int> node_phys_;
    std::unordered_map<ProcId, Proc> procs_;
    // Connections that have not yet completed RT_HELLO.
    std::unordered_map<StreamChannel*, std::unique_ptr<StreamChannel>> greeting_;
    std::unordered_map<int, uint64_t> resident_; // phys node -> pages
    std::vector<uint8_t> zero_page_;
    uint64_t next_zap_ = 1;
    Fd uffd_dev_;
};

} // namespace dnuma
