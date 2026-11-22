/* SPDX-License-Identifier: GPL-2.0-only */
// dnuma-master: the single authority for a dnuma system.
//
// Split across several translation units:
//   master.cc        - startup, message routing, CoherenceIo
//   master_slaves.cc - attach, lease, failure, fencing, draining
//   master_proc.cc   - logical processes, execution contexts, application calls
//   master_ctl.cc    - dnumactl command handling
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/hwprobe.h"
#include "common/metrics.h"
#include "common/reactor.h"
#include "common/sha256.h"
#include "master/coherence.h"
#include "master/export_shards.h"
#include "master/kernel_view.h"
#include "master/lifecycle.h"
#include "master/topology.h"
#include "node/link_table.h"
#include "node/node_core.h"
#include "node/page_pool.h"
#include "transport/messages.h"
#include "transport/rdma.h"
#include "transport/stream.h"

namespace dnuma {

struct MasterConfig {
    std::string ctl_sock = "/run/dnuma/master.sock";
    std::string app_sock = "/run/dnuma/app.sock";
    std::string key_file = "/etc/dnuma/cluster.key";
    uint16_t agent_port = 7710;
    uint64_t pool_bytes = 256ull << 20;
    uint64_t capacity_bytes = 0; // 0 = 50% of MemAvailable at start
    uint32_t lease_ms = 5000;
    uint32_t renew_ms = 1000;
    uint64_t attach_timeout_ms = 20000;
    uint64_t fence_ack_timeout_ms = 3000;
    uint64_t op_timeout_ms = 30000;
    uint64_t va_base = kDefaultVaBase;
    uint64_t va_size = kDefaultVaSize;
    uint64_t default_stack_bytes = 256 * 1024;
    uint32_t drain_window = 64; // concurrent page evacuations
    // Process export (docs/export.md)
    bool export_auto = false;       // move CPU-bound processes to idle slave CPUs automatically
    uint32_t export_uid_min = 1000; // never touch processes of users below this uid
    uint32_t export_tick_ms = 250;
    unsigned export_workers = 0;    // threads serving exported processes (0 = one per 6 CPUs)
    bool kernel_view = true;        // show slave CPUs and exported work in /proc if the kernel can
    bool view_online = true;        // ... and list slave CPUs in /sys/devices/system/cpu/online
};

// One dnumactl connection. Long operations keep a reference and stream
// progress; the session may outlive the client (output is then dropped).
struct CtlSession {
    std::unique_ptr<StreamChannel> chan;
    bool finished = false;
    void out(const std::string& text);
    void done(int code);
};
using CtlPtr = std::shared_ptr<CtlSession>;

struct DrainOp {
    CtlPtr ctl;
    enum class Phase { Exec, Pages, Images, Detach } phase = Phase::Exec;
    uint64_t t0 = 0, deadline_ns = 0;
    Reactor::TimerId deadline_timer = 0, progress_timer = 0, detach_timer = 0;
    std::deque<std::pair<ProcId, PageNo>> work;
    uint32_t inflight = 0;
    uint64_t moved = 0, handed_off = 0, dropped = 0;
    uint64_t rescans = 0, last_resident = UINT64_MAX;
    Status error;
    bool pumping = false; // re-entrancy guard for the evacuation loop
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

struct Slave {
    std::string name;      // administrative name (as given to `slave connect`)
    std::string address;   // management address override, if any
    uint16_t port = 0;
    MachineId machine = kNoMachine;
    std::string uuid, hostname, peer_ip;
    SlaveState state = SlaveState::Disconnected;
    Incarnation inc = 0;
    FenceProof proof = FenceProof::None;
    bool fence_pending = false; // an old incarnation is not yet proven stopped
    bool ever_leased = false;
    std::string last_error;
    std::deque<std::string> history; // recent lifecycle transitions (display only)

    std::unique_ptr<StreamChannel> mgmt;
    MachineResources res;
    std::string rdma_ip;
    uint16_t rdma_port = 0;
    uint64_t pool_bytes = 0, capacity_pages = 0;
    std::vector<NodeAssignEntry> nodes;
    ResourceReportMsg report;
    uint64_t report_ns = 0;
    uint32_t stale_images = 0, stale_contexts = 0;

    uint64_t last_renew_ns = 0;
    uint64_t fence_safe_ns = 0; // after this (master clock) the agent's lease has expired
    Reactor::TimerId lease_timer = 0, attach_timer = 0, fence_timer = 0;
    uint64_t attach_ns = 0, attaches = 0;
    std::vector<uint8_t> nonce_m;
    CtlPtr attach_ctl, fence_ctl;
    std::set<MachineId> peers_pending;
    bool rdma_ready = false; // link established and verified in both directions
    std::string attach_stage;
    std::unique_ptr<DrainOp> drain;
    std::vector<std::string> killed; // processes terminated by the last fence
    int cpu_base = -1;         // first CPU number this slave's CPUs are presented as
    int view_cpus = 0;         // how many of its CPUs the kernel's view currently shows
    std::string export_block;  // why native processes cannot be exported here (empty = they can)
};

// How often agents sample their CPUs and bodies for the master's /proc view.
// top's default refresh is 3 s; at this rate the figures it shows are within
// a sixth of that of the truth.
inline constexpr uint32_t kViewSampleMs = 500;

struct ExecCtx {
    CtxId id = 0;
    ProcId proc = 0;
    MachineId machine = kNoMachine;
    NodeId node = kNoNode;
    uint64_t entry = 0, arg = 0;
    PageNo stack_first = 0;
    uint64_t stack_pages = 0;
    enum class State { Creating, Running, Suspending, Moving, Exited } state = State::Creating;
    ThreadInfo info;
    bool auto_place = false;
    CtxId peer = 0;
    uint64_t peer_sample_ns = 0;
    double handoff_ns = 0;
    NodeId migrate_node = kNoNode;
    uint64_t saved_sp = 0;
    uint64_t t_migrate = 0;
    // (origin machine, call id) of callers waiting in thread_create / thread_join
    std::vector<std::pair<MachineId, uint64_t>> creators, joiners;
};

struct Process {
    ProcId id = 0;
    int home_pid = 0;
    std::string exe, name;
    Digest exe_hash{};
    std::vector<std::string> argv;
    uint32_t uid = 0, gid = 0;
    uint64_t canary = 0;
    bool home_ready = false, dying = false;
    uint64_t start_ns = 0;
    enum class ImageState { Creating, Ready, Destroying };
    std::map<MachineId, ImageState> images;
    std::map<MachineId, std::vector<std::function<void(Status)>>> image_waiters;
    std::map<CtxId, ExecCtx> ctxs;
    uint64_t image_flags = 0;
};

class Master : public CoherenceIo {
public:
    Master(Reactor& reactor, MasterConfig cfg);
    ~Master() override;
    Status init();
    void shutdown();
    // Brings exported processes home first, then shuts down and calls `done`.
    void request_shutdown(std::function<void()> done);
    void on_sigchld();

    // CoherenceIo
    void send_page(MachineId to, MsgType type, const PageMsg& m) override;
    Incarnation incarnation_of(MachineId m) override;
    bool machine_usable(MachineId m) override;
    void page_handoff(ProcId proc, CtxId ctx, MachineId from, CtxId peer, MachineId to, uint64_t cost) override;
    void coherence_failure(ProcId proc, const std::string& why) override;

private:
    friend struct CtlSession;

    // ---- routing (master.cc) ----
    void send_to_machine(MachineId m, Msg msg);
    void on_node_msg(MachineId from, Msg&& m);
    Slave* slave_by_machine(MachineId m);
    Slave* slave_by_name(const std::string& name);
    std::string machine_name(MachineId m); // administrative name
    std::string machine_host(MachineId m); // physical hostname
    void transition(Slave& s, LifecycleEvent ev, const std::string& why);
    uint64_t capacity_pages(MachineId m);
    uint64_t free_pages(MachineId m);

    // ---- slaves (master_slaves.cc) ----
    void slave_connect(const std::string& name, const std::string& address, uint16_t port, CtlPtr ctl);
    void attach_fail(Slave& s, const Status& st);
    void on_mgmt_msg(Slave& s, Msg&& m);
    void on_hello_ack(Slave& s, const Msg& m);
    void on_attach_ack(Slave& s, const Msg& m);
    void attach_rdma(Slave& s);
    void attach_peers(Slave& s);
    void attach_maybe_complete(Slave& s);
    void on_link_verify_ack(Slave& s, const SimpleMsg& ack);
    void on_mgmt_closed(const std::string& name, Incarnation inc, const Status& st);
    void drop_mgmt(Slave& s);
    void on_heartbeat(Slave& s, const Msg& m);
    void arm_lease_timer(Slave& s);
    void slave_failed(Slave& s, const std::string& reason);
    void begin_fence(Slave& s, const std::string& cause, CtlPtr ctl);
    void fence_finish(Slave& s, bool confirmed, const std::string& how);
    std::vector<ProcId> dependents_of(MachineId m);
    void hard_disconnect(Slave& s, bool dry_run, CtlPtr ctl);
    void drain_start(Slave& s, uint64_t timeout_ms, CtlPtr ctl);
    void drain_step(MachineId m);
    void drain_pages(Slave& s);
    void drain_fail(Slave& s, const Status& st);
    void drain_finish(Slave& s);
    std::string drain_progress(Slave& s);
    MachineId pick_evacuation_target(MachineId avoid, uint64_t* budget);
    NodeId pick_node_on(MachineId m);
    void teardown_slave_links(Slave& s);

    // ---- processes (master_proc.cc) ----
    ProcId register_home(const RtHelloMsg& hello, int pid, uint32_t uid, uint32_t gid, uint64_t* va_base,
                         uint64_t* va_size, std::string* err);
    Process* find_proc(ProcId id);
    void destroy_process(ProcId id, const std::string& reason, bool forced);
    void on_image_ready(MachineId from, const Msg& m);
    void on_image_exited(MachineId from, const Msg& m);
    void ensure_image(Process& p, MachineId m, std::function<void(Status)> done);
    void on_app_call(MachineId from, const Msg& m);
    void app_reply(const AppCallMsg& call, int status, const std::string& error, std::vector<uint8_t> body);
    void on_exec_event(MachineId from, const Msg& m);
    void on_syscall(MachineId from, const Msg& m);
    void start_ctx(Process& p, ExecCtx& c);
    Status request_ctx_migration(Process& p, ExecCtx& c, NodeId dest);
    void resume_ctx(Process& p, ExecCtx& c);
    void finish_ctx_exit(Process& p, CtxId id);
    Status resolve_node(int logical, bool for_placement, const NodeRecord** out);
    void do_alloc(Process& p, const AppCallMsg& call);
    void do_thread_create(Process& p, const AppCallMsg& call);
    void do_migrate_pages(Process& p, const AppCallMsg& call);

    // ---- process export (master_export.cc) ----
    void export_init();
    void export_tick();
    void export_slave_attached(Slave& s);
    Slave* export_target(const std::string& name, std::string* why, bool require_idle = true);
    int export_pick_cpu(Slave& s);
    Status export_pid(pid_t pid, Slave& s, ExportPlacement placement, int cluster_cpu = -1);
    std::string render_exports();

    // ---- the kernel's /proc view of slave CPUs and exported work (master_view.cc) ----
    void view_init();
    void view_slave_attached(Slave& s);
    void view_slave_gone(Slave& s);
    void on_view_sample(Slave& s, const ViewSampleMsg& m);
    void view_task_started(uint64_t xid, pid_t pid, int cpu, bool pinned);
    void view_task_ended(uint64_t xid, pid_t pid);
    void view_task_exited(pid_t pid);
    std::string render_view();

    // ---- control (master_ctl.cc) ----
    void on_ctl_accept();
    void on_ctl_request(CtlPtr ctl, const std::vector<std::string>& args);
    std::string render_slave_list();
    std::string render_slave_status(Slave& s);
    std::string render_topology();
    std::string render_memory(const std::string& only_slave);
    std::string render_processes(const std::string& only_slave);
    std::string render_metrics(const std::string& only_slave);

    Reactor& reactor_;
    MasterConfig cfg_;
    std::vector<uint8_t> key_;
    uint64_t instance_ = 0;
    uint64_t next_inc_ = 0;
    Metrics metrics_;      // coordinator-level metrics
    Metrics node_metrics_; // metrics of the master's own node (machine 0)
    MachineResources res_;
    uint64_t local_capacity_pages_ = 0;

    Fd ctl_listen_, app_listen_;
    std::set<CtlPtr> ctl_sessions_;
    RdmaEnv rdma_;
    PagePool pool_;
    LinkTable links_;
    std::unique_ptr<NodeCore> node_;
    Topology topo_;
    Coherence coh_;

    std::map<std::string, std::unique_ptr<Slave>> slaves_; // by administrative name
    std::map<std::string, MachineId> uuid_machine_;
    MachineId next_machine_ = 1;

    std::map<ProcId, Process> procs_;
    ProcId next_proc_ = 1;
    CtxId next_ctx_ = 1;
    std::deque<std::string> kill_log_; // recent forced terminations (display only)
    std::map<uint64_t, std::function<void(const MemoryMsg&)>> pending_mem_;
    uint64_t next_req_ = 1;
    std::map<std::string, CtlPtr> fault_ctl_;
    bool shutting_down_ = false;
    std::unique_ptr<ExportShards> exports_;
    std::map<pid_t, uint64_t> export_cputime_; // balancer: CPU time seen at the last tick
    std::map<pid_t, uint64_t> export_skip_;    // pid -> start time of processes not to try again
    std::function<void()> shutdown_done_;
    std::map<std::string, std::pair<int, int>> cpu_base_by_uuid_; // sticky CPU numbering: first number, how many
    int next_cpu_base_ = 0;

    // CPU time an exported process has consumed on slaves, as shown in its
    // /proc/<pid>/stat. Kept from its first export until it exits, so that
    // the total never goes backwards when it comes home or is exported again.
    struct ViewTask {
        Fd pidfd;                 // readable when the process has exited
        uint64_t xid = 0;         // export the current figures belong to
        uint64_t base_utime = 0, base_stime = 0; // earlier exports (local clock ticks)
        uint64_t cur_utime = 0, cur_stime = 0;   // the current one
        int cpu = -1;             // CPU it is presented on while exported
    };
    KernelView view_;
    std::map<pid_t, ViewTask> view_tasks_;
    std::map<std::string, int> phys_shift_by_uuid_; // sticky socket numbering in /proc/cpuinfo
    int next_phys_shift_ = 0;
};

} // namespace dnuma
