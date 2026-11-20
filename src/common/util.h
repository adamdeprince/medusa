/* SPDX-License-Identifier: GPL-2.0-only */
// Small RAII and system helpers used everywhere.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <time.h>
#include <unistd.h>

#include "common/status.h"

namespace dnuma {

// Owning file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release() {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// Monotonic nanoseconds. All latency measurement uses this clock; wall clocks
// on different machines are never compared.
inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

Status read_file(const std::string& path, std::string* out);
// Reads a small sysfs/procfs attribute and trims trailing whitespace; returns
// an empty string if the file cannot be read.
std::string read_attr(const std::string& path);
Status write_file_atomic(const std::string& path, const std::string& data, int mode);

std::vector<std::string> split(const std::string& s, char sep, bool keep_empty = false);
std::string trim(const std::string& s);
std::string hex(const uint8_t* p, size_t n);
bool unhex(const std::string& s, std::vector<uint8_t>* out);

// Parses "0-3,8,10-11" (kernel cpulist format).
std::vector<int> parse_cpulist(const std::string& s);
std::string format_cpulist(const std::vector<int>& cpus);

// Parses sizes like "64M", "2G", "4096"; and durations like "300s", "5m", "250ms".
bool parse_size(const std::string& s, uint64_t* out);
bool parse_duration_ms(const std::string& s, uint64_t* out);
std::string human_bytes(uint64_t bytes);
std::string with_commas(uint64_t v);

Status random_bytes(void* buf, size_t n);
uint64_t random_u64();
std::string make_uuid();

// Scheduling for processes that act on behalf of others.
//
// The daemons service page faults and system calls for other processes, and
// the home side of an exported process only executes short kernel operations
// for its body. On a machine with more runnable work than CPUs - the very
// situation process export exists for - a normal task that wakes up waits for
// the running task to finish its time slice, which adds milliseconds to every
// such operation. These tasks therefore get the highest ordinary priority
// (nice -20).
//
// Optionally (set_service_realtime) they run in the lowest real-time class
// instead (SCHED_FIFO priority 1, not inherited by children), like kernel
// threads that must not be delayed by user computation. That is off unless
// asked for: a real-time task that misbehaves can starve the kernel's own
// per-CPU threads and with them the whole machine, which no latency gain
// justifies by default.
struct SchedState {
    std::vector<uint8_t> attr; // struct sched_attr as returned by the kernel; empty = unknown
};
void set_service_realtime(bool on); // before any thread is started
bool service_realtime();
// For the calling thread.
void raise_priority();

// The daemons move large buffers (page batches, I/O data) at a high rate. By
// default the C library returns such buffers to the kernel when they are
// freed, so every message would pay for mapping, zero-filling and unmapping
// fresh pages. This keeps freed memory for reuse instead.
void tune_allocator();
// For another process: saves its current settings in *saved first.
void raise_priority_of(pid_t pid, SchedState* saved);
void restore_priority_of(pid_t pid, const SchedState& saved);

// Reads the pre-shared cluster key used to authenticate masters and agents.
Status load_cluster_key(const std::string& path, std::vector<uint8_t>* key);

Status set_nonblock(int fd, bool on);
Status set_cloexec(int fd);

} // namespace dnuma
