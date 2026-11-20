/* SPDX-License-Identifier: GPL-2.0-only */
// Structured error handling: a Status is an errno-style code plus context.
#pragma once

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <string>
#include <utility>

namespace dnuma {

class [[nodiscard]] Status {
public:
    Status() = default;
    Status(int code, std::string msg) : code_(code), msg_(std::move(msg)) {}

    static Status ok() { return {}; }
    // Capture errno with a formatted prefix.
    static Status sys(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static Status err(int code, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    bool is_ok() const { return code_ == 0; }
    explicit operator bool() const { return code_ == 0; }
    int code() const { return code_; }
    const std::string& message() const { return msg_; }
    std::string str() const {
        if (code_ == 0) return "ok";
        return msg_ + " (" + std::strerror(code_) + ")";
    }

private:
    int code_ = 0;
    std::string msg_;
};

std::string vformat(const char* fmt, va_list ap);
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#define DNUMA_TRY(expr)                    \
    do {                                   \
        ::dnuma::Status _st = (expr);      \
        if (!_st.is_ok()) return _st;      \
    } while (0)

} // namespace dnuma
