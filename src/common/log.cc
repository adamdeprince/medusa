// SPDX-License-Identifier: GPL-2.0-only
#include "common/log.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "common/status.h"
#include "common/util.h"

namespace dnuma {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::atomic<bool> g_page_trace{false};
std::atomic<int> g_fd{2};
char g_component[32] = "dnuma";
const char* level_tag(LogLevel l) {
    switch (l) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Trace: return "TRACE";
    }
    return "?";
}
} // namespace

void log_init(const char* component, LogLevel level) {
    snprintf(g_component, sizeof g_component, "%s", component);
    g_level.store(static_cast<int>(level));
}
void log_set_level(LogLevel level) { g_level.store(static_cast<int>(level)); }
LogLevel log_level() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }
void log_set_page_trace(bool on) { g_page_trace.store(on); }
bool log_page_trace() { return g_page_trace.load(std::memory_order_relaxed); }

bool log_parse_level(const std::string& s, LogLevel* out) {
    if (s == "error") *out = LogLevel::Error;
    else if (s == "warn") *out = LogLevel::Warn;
    else if (s == "info") *out = LogLevel::Info;
    else if (s == "debug") *out = LogLevel::Debug;
    else if (s == "trace") *out = LogLevel::Trace;
    else return false;
    return true;
}

bool log_to_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    g_fd.store(fd);
    return true;
}

void log_write(LogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string body = vformat(fmt, ap);
    va_end(ap);
    uint64_t t = now_ns();
    std::string line = format("%7llu.%06llu %s %s[%d] ", (unsigned long long)(t / 1000000000ull),
                              (unsigned long long)(t % 1000000000ull / 1000), level_tag(level),
                              g_component, int(getpid()));
    line += body;
    line += '\n';
    // One write() per line keeps concurrent writers from interleaving.
    ssize_t r = ::write(g_fd.load(), line.data(), line.size());
    (void)r;
}

} // namespace dnuma
