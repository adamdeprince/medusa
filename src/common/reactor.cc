// SPDX-License-Identifier: GPL-2.0-only
#include "common/reactor.h"

#include <cstdlib>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include "common/log.h"

namespace dnuma {

Reactor::Reactor() {
    epfd_.reset(epoll_create1(EPOLL_CLOEXEC));
    timerfd_.reset(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    eventfd_.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!epfd_.valid() || !timerfd_.valid() || !eventfd_.valid()) {
        LOG_ERROR("reactor: cannot create kernel objects: %s", strerror(errno));
        abort();
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = timerfd_.get();
    epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, timerfd_.get(), &ev);
    ev.data.fd = eventfd_.get();
    epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, eventfd_.get(), &ev);
}

Reactor::~Reactor() = default;

Status Reactor::add_fd(int fd, uint32_t events, FdHandler handler) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) return Status::sys("epoll add fd %d", fd);
    handlers_[fd] = std::make_shared<FdHandler>(std::move(handler));
    return Status::ok();
}

Status Reactor::mod_fd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) != 0) return Status::sys("epoll mod fd %d", fd);
    return Status::ok();
}

void Reactor::del_fd(int fd) {
    if (handlers_.erase(fd)) epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
}

Reactor::TimerId Reactor::add_timer(uint64_t delay_ns, Task task, uint64_t repeat_ns) {
    TimerId id = next_timer_++;
    uint64_t deadline = now_ns() + delay_ns;
    timers_.emplace(deadline, Timer{id, std::move(task), repeat_ns});
    timer_deadline_[id] = deadline;
    arm_timerfd();
    return id;
}

void Reactor::cancel_timer(TimerId id) {
    auto it = timer_deadline_.find(id);
    if (it == timer_deadline_.end()) return;
    auto range = timers_.equal_range(it->second);
    for (auto t = range.first; t != range.second; ++t) {
        if (t->second.id == id) {
            timers_.erase(t);
            break;
        }
    }
    timer_deadline_.erase(it);
    arm_timerfd();
}

void Reactor::arm_timerfd() {
    itimerspec its{};
    if (!timers_.empty()) {
        uint64_t d = timers_.begin()->first;
        if (d == 0) d = 1; // an all-zero it_value would disarm the timer
        its.it_value.tv_sec = time_t(d / 1000000000ull);
        its.it_value.tv_nsec = long(d % 1000000000ull);
    }
    timerfd_settime(timerfd_.get(), TFD_TIMER_ABSTIME, &its, nullptr);
}

void Reactor::run_timers() {
    uint64_t expirations;
    ssize_t r = ::read(timerfd_.get(), &expirations, sizeof expirations);
    (void)r;
    uint64_t now = now_ns();
    // Collect first: a timer task may add or cancel timers.
    std::vector<Timer> due;
    while (!timers_.empty() && timers_.begin()->first <= now) {
        due.push_back(std::move(timers_.begin()->second));
        timers_.erase(timers_.begin());
    }
    for (Timer& t : due) {
        if (t.repeat_ns == 0) timer_deadline_.erase(t.id);
    }
    for (Timer& t : due) {
        if (t.repeat_ns) {
            // Skip if it was cancelled by an earlier task in this batch.
            if (!timer_deadline_.count(t.id)) continue;
            Task copy = t.task;
            uint64_t deadline = now + t.repeat_ns;
            timer_deadline_[t.id] = deadline;
            timers_.emplace(deadline, Timer{t.id, std::move(t.task), t.repeat_ns});
            copy();
        } else {
            t.task();
        }
    }
    arm_timerfd();
}

void Reactor::post(Task task) {
    {
        std::lock_guard<std::mutex> lk(post_mu_);
        posted_.push_back(std::move(task));
    }
    uint64_t one = 1;
    ssize_t r = ::write(eventfd_.get(), &one, sizeof one);
    (void)r;
}

void Reactor::run_posted() {
    uint64_t v;
    ssize_t r = ::read(eventfd_.get(), &v, sizeof v);
    (void)r;
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lk(post_mu_);
        tasks.swap(posted_);
    }
    for (Task& t : tasks) t();
}

void Reactor::run() {
    std::vector<epoll_event> events(256);
    while (!stop_) {
        int n = epoll_wait(epfd_.get(), events.data(), int(events.size()), -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("reactor: epoll_wait: %s", strerror(errno));
            abort();
        }
        for (int i = 0; i < n && !stop_; ++i) {
            int fd = events[size_t(i)].data.fd;
            uint64_t t0 = slow_hook_ ? now_ns() : 0;
            const char* kind = fd == timerfd_.get() ? "timer" : fd == eventfd_.get() ? "posted task" : "fd handler";
            struct Timing {
                Reactor& r;
                uint64_t t0;
                const char* kind;
                int fd;
                ~Timing() {
                    if (!r.slow_hook_) return;
                    uint64_t d = now_ns() - t0;
                    if (d >= r.slow_ns_) r.slow_hook_(kind, fd, d);
                }
            } timing{*this, t0, kind, fd};
            if (fd == timerfd_.get()) {
                run_timers();
            } else if (fd == eventfd_.get()) {
                run_posted();
            } else {
                // Look the handler up at dispatch time: an earlier handler in
                // this batch may have removed this fd.
                auto it = handlers_.find(fd);
                if (it == handlers_.end()) continue;
                std::shared_ptr<FdHandler> h = it->second;
                (*h)(events[size_t(i)].events);
            }
        }
        if (size_t(n) == events.size()) events.resize(events.size() * 2);
    }
}

void Reactor::stop() {
    stop_ = true;
    uint64_t one = 1;
    ssize_t r = ::write(eventfd_.get(), &one, sizeof one);
    (void)r;
}

} // namespace dnuma
