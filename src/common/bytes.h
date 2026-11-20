/* SPDX-License-Identifier: GPL-2.0-only */
// Bounds-checked little-endian serialization used by the wire protocol.
// A Reader never reads past its buffer: any short read latches an error and
// returns zero values, and the caller checks ok() once at the end.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dnuma {

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { raw(&v, sizeof v); }
    void u32(uint32_t v) { raw(&v, sizeof v); }
    void u64(uint64_t v) { raw(&v, sizeof v); }
    void i32(int32_t v) { raw(&v, sizeof v); }
    void i64(int64_t v) { raw(&v, sizeof v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void bytes(const std::vector<uint8_t>& b) {
        u32(static_cast<uint32_t>(b.size()));
        raw(b.data(), b.size());
    }
    void raw(const void* p, size_t n) {
        if (n == 0) return;
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    void strs(const std::vector<std::string>& v) {
        u32(static_cast<uint32_t>(v.size()));
        for (const auto& s : v) str(s);
    }
    void u32s(const std::vector<uint32_t>& v) {
        u32(static_cast<uint32_t>(v.size()));
        for (uint32_t x : v) u32(x);
    }
    void i32s(const std::vector<int>& v) {
        u32(static_cast<uint32_t>(v.size()));
        for (int x : v) i32(x);
    }
    std::vector<uint8_t> take() { return std::move(buf_); }
    const std::vector<uint8_t>& data() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    explicit Reader(const std::vector<uint8_t>& v) : Reader(v.data(), v.size()) {}

    bool ok() const { return ok_; }
    // True when the whole buffer was consumed without error.
    bool done() const { return ok_ && p_ == end_; }
    size_t remaining() const { return size_t(end_ - p_); }

    uint8_t u8() { return get<uint8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    uint64_t u64() { return get<uint64_t>(); }
    int32_t i32() { return get<int32_t>(); }
    int64_t i64() { return get<int64_t>(); }
    bool boolean() { return u8() != 0; }
    std::string str() {
        uint32_t n = u32();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    std::vector<uint8_t> bytes() {
        uint32_t n = u32();
        if (!need(n)) return {};
        std::vector<uint8_t> v(p_, p_ + n);
        p_ += n;
        return v;
    }
    bool raw(void* out, size_t n) {
        if (!need(n)) return false;
        memcpy(out, p_, n);
        p_ += n;
        return true;
    }
    std::vector<std::string> strs() {
        uint32_t n = count(4);
        std::vector<std::string> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n && ok_; ++i) v.push_back(str());
        return v;
    }
    std::vector<uint32_t> u32s() {
        uint32_t n = count(4);
        std::vector<uint32_t> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n && ok_; ++i) v.push_back(u32());
        return v;
    }
    std::vector<int> i32s() {
        uint32_t n = count(4);
        std::vector<int> v;
        v.reserve(n);
        for (uint32_t i = 0; i < n && ok_; ++i) v.push_back(i32());
        return v;
    }
    // Reads an element count and rejects counts that cannot possibly fit in
    // the remaining bytes (each element needs at least min_elem_size bytes),
    // so a hostile length can never drive a huge allocation.
    uint32_t count(size_t min_elem_size) {
        uint32_t n = u32();
        if (!ok_) return 0;
        if (min_elem_size && n > remaining() / min_elem_size) {
            ok_ = false;
            return 0;
        }
        return n;
    }

private:
    bool need(size_t n) {
        if (!ok_ || size_t(end_ - p_) < n) {
            ok_ = false;
            return false;
        }
        return true;
    }
    template <typename T> T get() {
        T v{};
        if (!need(sizeof(T))) return v;
        memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

} // namespace dnuma
