/* SPDX-License-Identifier: GPL-2.0-only */
// Leveled logging with a monotonic timestamp. Page-ownership tracing is a
// separate switch so it can be enabled without drowning in debug output.
#pragma once

#include <cstdarg>
#include <string>

namespace dnuma {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

void log_init(const char* component, LogLevel level);
void log_set_level(LogLevel level);
bool log_parse_level(const std::string& s, LogLevel* out);
LogLevel log_level();
void log_set_page_trace(bool on);
bool log_page_trace();
// Redirects log output to a file (append). Returns false if it cannot be opened.
bool log_to_file(const std::string& path);

void log_write(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define DLOG(level, ...)                                                        \
    do {                                                                        \
        if (static_cast<int>(level) <= static_cast<int>(::dnuma::log_level()))  \
            ::dnuma::log_write(level, __VA_ARGS__);                             \
    } while (0)

#define LOG_ERROR(...) DLOG(::dnuma::LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...) DLOG(::dnuma::LogLevel::Warn, __VA_ARGS__)
#define LOG_INFO(...) DLOG(::dnuma::LogLevel::Info, __VA_ARGS__)
#define LOG_DEBUG(...) DLOG(::dnuma::LogLevel::Debug, __VA_ARGS__)
#define LOG_TRACE(...) DLOG(::dnuma::LogLevel::Trace, __VA_ARGS__)

// One line per page-ownership transition (docs/memory.md, "Tracing").
#define PAGE_TRACE(...)                                                         \
    do {                                                                        \
        if (::dnuma::log_page_trace()) ::dnuma::log_write(::dnuma::LogLevel::Info, __VA_ARGS__); \
    } while (0)

} // namespace dnuma
