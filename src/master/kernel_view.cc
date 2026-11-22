// SPDX-License-Identifier: GPL-2.0-only
#include "master/kernel_view.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "common/log.h"

namespace dnuma {

namespace {
const char kPath[] = "/proc/dnuma_view";
// The kernel accepts at most this much in one write (DV_MAX_WRITE).
constexpr size_t kMaxWrite = 1024 * 1024;

// "key\t: value" -> value, if the line's key is `key`.
bool cpuinfo_value(const std::string& line, const char* key, std::string* value) {
    size_t n = strlen(key);
    if (line.compare(0, n, key) != 0) return false;
    size_t colon = line.find(':', n);
    if (colon == std::string::npos || trim(line.substr(n, colon - n)) != "") return false;
    *value = trim(line.substr(colon + 1));
    return true;
}
} // namespace

bool KernelView::open(std::string* why) {
    Fd fd(::open(kPath, O_RDWR | O_CLOEXEC));
    if (!fd.valid()) {
        if (errno == ENOENT) *why = "this kernel has no dnuma view (CONFIG_DNUMA_VIEW)";
        else if (errno == EBUSY) *why = "another manager holds /proc/dnuma_view";
        else *why = format("cannot open %s: %s", kPath, strerror(errno));
        return false;
    }
    std::string text;
    char buf[512];
    ssize_t n;
    while ((n = read(fd.get(), buf, sizeof buf)) > 0) text.append(buf, size_t(n));
    int version = 0;
    can_online_ = can_pin_ = false;
    for (const std::string& line : split(text, '\n')) {
        std::vector<std::string> f = split(line, ' ');
        if (f.size() != 2) continue;
        if (f[0] == "version") version = atoi(f[1].c_str());
        else if (f[0] == "nr_cpu_ids") nr_cpu_ids_ = atoi(f[1].c_str());
        else if (f[0] == "user_hz") user_hz_ = atoi(f[1].c_str());
        else if (f[0] == "online") can_online_ = true;
        else if (f[0] == "affinity_query" && f[1] == "1") can_pin_ = true;
    }
    if (version != 1 || nr_cpu_ids_ <= 0 || user_hz_ <= 0) {
        *why = format("%s speaks version %d of an interface this program knows as version 1", kPath, version);
        return false;
    }
    fd_ = std::move(fd);
    return true;
}

void KernelView::set_cpu(int id, const std::string& block) {
    if (!active()) return;
    pending_.push_back(format("cpu %d info %zu\n", id, block.size()) + block);
}

void KernelView::cpu_stat(int id, const uint64_t v[kViewStatFields]) {
    if (!active()) return;
    std::string cmd = format("cpu %d stat", id);
    for (int i = 0; i < kViewStatFields; ++i) cmd += format(" %llu", (unsigned long long)v[i]);
    pending_.push_back(cmd + "\n");
}

void KernelView::del_cpu(int id) {
    if (active()) pending_.push_back(format("cpu %d del\n", id));
}

void KernelView::set_task(pid_t pid, int cpu, bool running, uint64_t utime, uint64_t stime) {
    if (!active()) return;
    pending_.push_back(format("task %d %d %c %llu %llu\n", int(pid), cpu, running ? 'R' : '-', (unsigned long long)utime,
                              (unsigned long long)stime));
}

void KernelView::set_online(bool on) {
    if (active() && can_online_) pending_.push_back(on ? "online on\n" : "online off\n");
}

void KernelView::report_pin(pid_t tid, int cpu) {
    if (active() && can_pin_) pending_.push_back(format("pin %d %d\n", int(tid), cpu));
}

void KernelView::del_task(pid_t pid) {
    if (active()) pending_.push_back(format("task %d del\n", int(pid)));
}

void KernelView::flush() {
    std::vector<std::string> cmds;
    cmds.swap(pending_);
    if (!active()) return;
    std::string batch;
    auto send = [&] {
        if (batch.empty()) return;
        // The kernel applies a batch in order and stops at the first command
        // it rejects; what follows that command in the batch is lost.
        if (write(fd_.get(), batch.data(), batch.size()) < 0) {
            if (errors_++ < 5) LOG_WARN("kernel view: a batch of %zu bytes was rejected: %s", batch.size(), strerror(errno));
        }
        batch.clear();
    };
    for (std::string& c : cmds) {
        if (c.size() > kMaxWrite) {
            if (errors_++ < 5) LOG_WARN("kernel view: a %zu-byte command exceeds what the kernel accepts in one write", c.size());
            continue;
        }
        if (batch.size() + c.size() > kMaxWrite) send();
        batch += c;
    }
    send();
}

std::vector<std::pair<int, std::string>> split_cpuinfo(const std::string& text) {
    std::vector<std::pair<int, std::string>> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find("\n\n", pos);
        end = end == std::string::npos ? text.size() : end + 2;
        std::string block = text.substr(pos, end - pos);
        pos = end;
        for (const std::string& line : split(block, '\n')) {
            std::string v;
            if (cpuinfo_value(line, "processor", &v)) {
                out.emplace_back(atoi(v.c_str()), std::move(block));
                break;
            }
        }
    }
    return out;
}

int max_physical_id(const std::string& cpuinfo) {
    int best = -1;
    for (const std::string& line : split(cpuinfo, '\n')) {
        std::string v;
        if (cpuinfo_value(line, "physical id", &v)) best = std::max(best, atoi(v.c_str()));
    }
    return best;
}

std::string remote_cpuinfo_block(const std::string& block, int id, int phys_shift, const std::string& host) {
    std::string out;
    for (const std::string& line : split(block, '\n')) {
        std::string v;
        if (cpuinfo_value(line, "processor", &v)) out += format("processor\t: %d\n", id);
        else if (cpuinfo_value(line, "physical id", &v)) out += format("physical id\t: %d\n", atoi(v.c_str()) + phys_shift);
        else out += line + "\n";
    }
    out += "dnuma host\t: " + host + "\n\n";
    return out;
}

} // namespace dnuma
