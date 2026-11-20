/* SPDX-License-Identifier: GPL-2.0-only */
// Single-threaded epoll reactor. Every daemon is one reactor: sockets, RDMA
// completion channels, userfaultfds, pidfds and timers are all event sources,
// so nothing in the daemons polls or sleeps.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "common/util.h"

namespace dnuma {

class Reactor {
public:
    using FdHandler = std::function<void(uint32_t events)>;
    using Task = std::function<void()>;
    using TimerId = uint64_t;

    Reactor();
    ~Reactor();
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    Status add_fd(int fd, uint32_t events, FdHandler handler);
    Status mod_fd(int fd, uint32_t events);
    void del_fd(int fd);

    // One-shot timer (repeat_ns == 0) or periodic timer.
    TimerId add_timer(uint64_t delay_ns, Task task, uint64_t repeat_ns = 0);
    void cancel_timer(TimerId id);

    // Runs the task on the reactor thread after the current event. Safe to
    // call from any thread.
    void post(Task task);

    void run();
    void stop();
    // Called for any handler, timer or posted task that runs longer than
    // `threshold_ns`: a single-threaded reactor must never stall.
    void set_slow_hook(uint64_t threshold_ns, std::function<void(const char* kind, int fd, uint64_t ns)> hook) {
        slow_ns_ = threshold_ns;
        slow_hook_ = std::move(hook);
    }
    bool stopping() const { return stop_; }

private:
    struct Timer {
        TimerId id;
        Task task;
        uint64_t repeat_ns;
    };
    void arm_timerfd();
    void run_timers();
    void run_posted();

    Fd epfd_, timerfd_, eventfd_;
    std::unordered_map<int, std::shared_ptr<FdHandler>> handlers_;
    std::multimap<uint64_t, Timer> timers_;
    std::unordered_map<TimerId, uint64_t> timer_deadline_;
    TimerId next_timer_ = 1;
    std::mutex post_mu_;
    std::vector<Task> posted_;
    bool stop_ = false;
    uint64_t slow_ns_ = 0;
    std::function<void(const char*, int, uint64_t)> slow_hook_;
};

} // namespace dnuma
