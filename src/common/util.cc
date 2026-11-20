// SPDX-License-Identifier: GPL-2.0-only
#include "common/util.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/random.h>
#include <malloc.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#include "common/log.h"
#include "common/types.h"

namespace dnuma {

std::string vformat(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return fmt;
    std::string out(size_t(n), '\0');
    vsnprintf(out.data(), size_t(n) + 1, fmt, ap);
    return out;
}

std::string format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    return s;
}

Status Status::sys(const char* fmt, ...) {
    int e = errno;
    va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    return Status(e ? e : EIO, std::move(s));
}

Status Status::err(int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    return Status(code ? code : EIO, std::move(s));
}

Status read_file(const std::string& path, std::string* out) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) return Status::sys("open %s", path.c_str());
    out->clear();
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::sys("read %s", path.c_str());
        }
        if (n == 0) break;
        out->append(buf, size_t(n));
    }
    return Status::ok();
}

std::string read_attr(const std::string& path) {
    std::string s;
    if (!read_file(path, &s).is_ok()) return {};
    return trim(s);
}

Status write_file_atomic(const std::string& path, const std::string& data, int mode) {
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
    if (!fd.valid()) return Status::sys("create %s", tmp.c_str());
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd.get(), data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            Status st = Status::sys("write %s", tmp.c_str());
            ::unlink(tmp.c_str());
            return st;
        }
        off += size_t(n);
    }
    if (::fsync(fd.get()) != 0 || ::rename(tmp.c_str(), path.c_str()) != 0) {
        Status st = Status::sys("commit %s", path.c_str());
        ::unlink(tmp.c_str());
        return st;
    }
    return Status::ok();
}

std::vector<std::string> split(const std::string& s, char sep, bool keep_empty) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t pos = s.find(sep, start);
        std::string part = s.substr(start, pos == std::string::npos ? pos : pos - start);
        if (keep_empty || !part.empty()) out.push_back(std::move(part));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string hex(const uint8_t* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(d[p[i] >> 4]);
        out.push_back(d[p[i] & 15]);
    }
    return out;
}

bool unhex(const std::string& s, std::vector<uint8_t>* out) {
    if (s.size() % 2) return false;
    out->clear();
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = val(s[i]), b = val(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out->push_back(uint8_t(a << 4 | b));
    }
    return true;
}

std::vector<int> parse_cpulist(const std::string& s) {
    std::vector<int> out;
    for (const std::string& part : split(trim(s), ',')) {
        size_t dash = part.find('-');
        char* end = nullptr;
        long a = strtol(part.c_str(), &end, 10);
        long b = a;
        if (dash != std::string::npos) b = strtol(part.c_str() + dash + 1, &end, 10);
        for (long c = a; c <= b && c >= 0; ++c) out.push_back(int(c));
    }
    return out;
}

std::string format_cpulist(const std::vector<int>& cpus) {
    std::string out;
    for (size_t i = 0; i < cpus.size();) {
        size_t j = i;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
        if (!out.empty()) out += ',';
        out += std::to_string(cpus[i]);
        if (j > i) out += '-' + std::to_string(cpus[j]);
        i = j + 1;
    }
    return out;
}

bool parse_size(const std::string& s, uint64_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return false;
    unsigned shift = 0;
    std::string suffix = trim(end);
    if (!suffix.empty()) {
        switch (suffix[0]) {
        case 'k': case 'K': shift = 10; break;
        case 'm': case 'M': shift = 20; break;
        case 'g': case 'G': shift = 30; break;
        case 't': case 'T': shift = 40; break;
        default: return false;
        }
    }
    if (shift && v > (UINT64_MAX >> shift)) return false;
    *out = uint64_t(v) << shift;
    return true;
}

bool parse_duration_ms(const std::string& s, uint64_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return false;
    std::string suffix = trim(end);
    uint64_t mult = 1000; // bare numbers are seconds
    if (suffix == "ms") mult = 1;
    else if (suffix == "s" || suffix.empty()) mult = 1000;
    else if (suffix == "m") mult = 60000;
    else if (suffix == "h") mult = 3600000;
    else return false;
    if (v > UINT64_MAX / mult) return false;
    *out = uint64_t(v) * mult;
    return true;
}

std::string human_bytes(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0) return format("%" PRIu64 " B", bytes);
    return format(v >= 100 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

std::string with_commas(uint64_t v) {
    std::string s = std::to_string(v);
    for (ssize_t i = ssize_t(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

Status random_bytes(void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return Status::sys("getrandom");
        }
        p += r;
        n -= size_t(r);
    }
    return Status::ok();
}

uint64_t random_u64() {
    uint64_t v = 0;
    if (!random_bytes(&v, sizeof v).is_ok()) abort(); // no entropy source: cannot run safely
    return v;
}

std::string make_uuid() {
    uint8_t b[16];
    if (!random_bytes(b, sizeof b).is_ok()) abort();
    b[6] = uint8_t((b[6] & 0x0f) | 0x40);
    b[8] = uint8_t((b[8] & 0x3f) | 0x80);
    std::string h = hex(b, 16);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) +
           "-" + h.substr(20, 12);
}

Status set_nonblock(int fd, bool on) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return Status::sys("F_GETFL");
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    if (fcntl(fd, F_SETFL, fl) < 0) return Status::sys("F_SETFL");
    return Status::ok();
}

Status set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0) return Status::sys("F_SETFD");
    return Status::ok();
}

namespace {
// struct sched_attr (include/uapi/linux/sched/types.h); only the first 48
// bytes (SCHED_ATTR_SIZE_VER0) are set by us, but the kernel may return more.
struct KSchedAttr {
    uint32_t size, sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
};
constexpr uint64_t kResetOnFork = 0x01; // SCHED_FLAG_RESET_ON_FORK

bool set_fifo(pid_t pid) {
    KSchedAttr a{};
    a.size = sizeof a;
    a.sched_policy = 1; // SCHED_FIFO
    a.sched_flags = kResetOnFork;
    a.sched_priority = 1;
    return syscall(SYS_sched_setattr, pid, &a, 0) == 0;
}
} // namespace

void tune_allocator() {
#ifdef __GLIBC__
    // The largest value the library accepts for the mmap threshold is 32 MiB
    // (HEAP_MAX_SIZE / 2); buffers here are far smaller.
    mallopt(M_MMAP_THRESHOLD, 32 << 20);
    mallopt(M_TRIM_THRESHOLD, 256 << 20);
    mallopt(M_TOP_PAD, 16 << 20);
#endif
}

namespace {
bool g_service_realtime = false;
}
void set_service_realtime(bool on) { g_service_realtime = on; }
bool service_realtime() { return g_service_realtime; }

void raise_priority() {
    if (g_service_realtime) {
        if (set_fifo(0)) return;
        LOG_WARN("cannot use a real-time scheduling class (%s); falling back to nice -20", strerror(errno));
    }
    // On Linux the nice value is per thread; 0 means the calling thread.
    if (setpriority(PRIO_PROCESS, 0, -20) != 0) LOG_WARN("cannot raise scheduling priority: %s", strerror(errno));
}

void raise_priority_of(pid_t pid, SchedState* saved) {
    saved->attr.assign(256, 0);
    if (syscall(SYS_sched_getattr, pid, saved->attr.data(), unsigned(saved->attr.size()), 0) != 0) saved->attr.clear();
    if (!g_service_realtime || !set_fifo(pid)) setpriority(PRIO_PROCESS, id_t(pid), -20);
}

void restore_priority_of(pid_t pid, const SchedState& saved) {
    if (saved.attr.empty()) return;
    std::vector<uint8_t> a = saved.attr;
    syscall(SYS_sched_setattr, pid, a.data(), 0);
}

Status load_cluster_key(const std::string& path, std::vector<uint8_t>* key) {
    std::string raw;
    DNUMA_TRY(read_file(path, &raw));
    raw = trim(raw);
    if (raw.size() < 32)
        return Status::err(EINVAL, "cluster key %s is too short (need at least 32 characters)", path.c_str());
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && (st.st_mode & 0077))
        LOG_WARN("cluster key %s is readable by group/others; it should be mode 0600", path.c_str());
    key->assign(raw.begin(), raw.end());
    return Status::ok();
}

const char* to_string(Access a) {
    switch (a) {
    case Access::None: return "none";
    case Access::Read: return "read";
    case Access::Write: return "write";
    }
    return "?";
}
const char* to_string(PageState s) {
    switch (s) {
    case PageState::Unbacked: return "UNBACKED";
    case PageState::Exclusive: return "EXCLUSIVE";
    case PageState::Shared: return "SHARED";
    case PageState::Lost: return "LOST";
    }
    return "?";
}
const char* to_string(SlaveState s) {
    switch (s) {
    case SlaveState::Disconnected: return "DISCONNECTED";
    case SlaveState::Connecting: return "CONNECTING";
    case SlaveState::Active: return "ACTIVE";
    case SlaveState::Draining: return "DRAINING";
    case SlaveState::Failed: return "FAILED";
    case SlaveState::Fencing: return "FENCING";
    case SlaveState::FenceIncomplete: return "FENCE_INCOMPLETE";
    }
    return "?";
}
const char* to_string(FenceProof p) {
    switch (p) {
    case FenceProof::None: return "none";
    case FenceProof::AgentConfirmed: return "agent-confirmed";
    case FenceProof::LeaseExpired: return "lease-expired";
    case FenceProof::CleanDetach: return "clean-detach";
    case FenceProof::Reattached: return "reattach-handshake";
    }
    return "?";
}
const char* to_string(RevokeMode m) {
    switch (m) {
    case RevokeMode::ExportOnly: return "export";
    case RevokeMode::DowngradeExport: return "downgrade+export";
    case RevokeMode::InvalidateExport: return "invalidate+export";
    case RevokeMode::Invalidate: return "invalidate";
    }
    return "?";
}
const char* to_string(GrantSource s) {
    switch (s) {
    case GrantSource::Zero: return "zero";
    case GrantSource::Local: return "local";
    case GrantSource::Remote: return "remote";
    }
    return "?";
}

} // namespace dnuma
