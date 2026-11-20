/* SPDX-License-Identifier: GPL-2.0-only */
// The master's side of the kernel's "dnuma view" (kernel/patches,
// CONFIG_DNUMA_VIEW): what makes the CPUs of slaves, and the work exported
// processes do on them, appear in /proc/cpuinfo, /proc/stat,
// /proc/<pid>/stat and (optionally) /sys/devices/system/cpu/online on the
// master, so that ordinary tools such as top show them. The kernel discards everything written here when the file is closed,
// so the view never outlives the master.
//
// On a kernel without the view every call is a no-op.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

#include "common/util.h"

namespace dnuma {

inline constexpr int kViewStatFields = 10; // the numbers of a "cpuN" line of /proc/stat

class KernelView {
public:
    // Opens /proc/dnuma_view for writing. Returns false and sets *why when
    // the running kernel has no view or another manager is using it.
    bool open(std::string* why);
    bool active() const { return fd_.valid(); }
    void close() { fd_ = Fd(); } // the kernel forgets everything written here
    int nr_cpu_ids() const { return nr_cpu_ids_; } // remote CPU numbers start here
    int user_hz() const { return user_hz_; }       // unit of every time passed in

    // The calls below queue commands; flush() sends them.
    void set_cpu(int id, const std::string& cpuinfo_block); // defines or replaces a CPU
    void cpu_stat(int id, const uint64_t v[kViewStatFields]);
    void del_cpu(int id);
    // cpu -1: report the CPU the task really last ran on. running: report it
    // as running while it sleeps here.
    void set_task(pid_t pid, int cpu, bool running, uint64_t utime, uint64_t stime);
    // Whether the CPUs defined here are also listed in sysfs "online", which
    // is where the C library's count of online processors comes from. A
    // kernel can have the view without that part (it is a patch of its own);
    // then can_set_online() is false and set_online() does nothing.
    bool can_set_online() const { return can_online_; }
    void set_online(bool on);
    void del_task(pid_t pid);
    void flush();

    uint64_t write_errors() const { return errors_; }

private:
    Fd fd_;
    int nr_cpu_ids_ = 0, user_hz_ = 100;
    bool can_online_ = false;
    std::vector<std::string> pending_; // one whole command each
    uint64_t errors_ = 0;
};

// Splits the text of /proc/cpuinfo into (processor number, block) pairs; a
// block keeps its trailing blank line.
std::vector<std::pair<int, std::string>> split_cpuinfo(const std::string& text);
// The highest "physical id" in /proc/cpuinfo text (-1 if there is none).
int max_physical_id(const std::string& cpuinfo);
// Rewrites one block for display on another machine: the processor gets the
// number `id`, its "physical id" is shifted by `phys_shift` (so sockets of
// different machines stay distinct), and a "dnuma host" line names the
// machine the CPU belongs to.
std::string remote_cpuinfo_block(const std::string& block, int id, int phys_shift, const std::string& host);

} // namespace dnuma
