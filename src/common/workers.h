/* SPDX-License-Identifier: GPL-2.0-only */
// A fixed set of worker threads for work that must not run on a daemon's
// event loop: anything whose duration depends on the amount of data. Jobs
// hand their results back with Reactor::post().
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace dnuma {

class WorkerPool {
public:
    // `thread_init` runs once on each worker thread before its first job.
    explicit WorkerPool(unsigned threads, std::function<void()> thread_init = {}) {
        if (threads == 0) threads = 1;
        for (unsigned i = 0; i < threads; ++i) {
            threads_.emplace_back([this, thread_init] {
                if (thread_init) thread_init();
                run();
            });
        }
    }
    // Jobs that have not started are dropped; running ones are waited for.
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : threads_) t.join();
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    // One worker per `cpus_per_worker` CPUs: service threads should be able to
    // keep up with the machine's own work without taking a large share of it.
    static unsigned default_threads(unsigned cpus) { return cpus / 6 > 0 ? cpus / 6 : 1; }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

} // namespace dnuma
