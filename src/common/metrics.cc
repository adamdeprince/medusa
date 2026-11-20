// SPDX-License-Identifier: GPL-2.0-only
#include "common/metrics.h"

#include <algorithm>

#include "common/status.h"
#include "common/util.h"

namespace dnuma {

size_t Histogram::bucket_of(uint64_t v) {
    if (v < 4) return size_t(v); // exact buckets for 0..3
    unsigned msb = 63 - unsigned(__builtin_clzll(v));
    unsigned sub = unsigned((v >> (msb - 2)) & 3);
    return size_t(msb) * 4 + sub;
}

uint64_t Histogram::bucket_upper(size_t b) {
    if (b < 4) return uint64_t(b);
    unsigned msb = unsigned(b / 4), sub = unsigned(b % 4);
    uint64_t base = 1ull << msb;
    uint64_t step = base / 4;
    uint64_t upper = base + step * (sub + 1) - 1;
    return upper < base ? UINT64_MAX : upper;
}

void Histogram::record(uint64_t v) {
    ++buckets_[bucket_of(v)];
    ++count_;
    sum_ += v;
    min_ = std::min(min_, v);
    max_ = std::max(max_, v);
}

void Histogram::merge(const Histogram& o) {
    for (size_t i = 0; i < kBuckets; ++i) buckets_[i] += o.buckets_[i];
    count_ += o.count_;
    sum_ += o.sum_;
    min_ = std::min(min_, o.min_);
    max_ = std::max(max_, o.max_);
}

uint64_t Histogram::quantile(double q) const {
    if (!count_) return 0;
    uint64_t target = uint64_t(q * double(count_));
    if (target >= count_) target = count_ - 1;
    uint64_t seen = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
        seen += buckets_[i];
        if (seen > target) return std::min(bucket_upper(i), max_);
    }
    return max_;
}

void Histogram::encode(Writer& w) const {
    w.u64(count_);
    w.u64(sum_);
    w.u64(min_);
    w.u64(max_);
    uint32_t nonzero = 0;
    for (uint64_t b : buckets_) nonzero += b ? 1 : 0;
    w.u32(nonzero);
    for (size_t i = 0; i < kBuckets; ++i) {
        if (!buckets_[i]) continue;
        w.u16(uint16_t(i));
        w.u64(buckets_[i]);
    }
}

bool Histogram::decode(Reader& r) {
    *this = Histogram();
    count_ = r.u64();
    sum_ = r.u64();
    min_ = r.u64();
    max_ = r.u64();
    uint32_t n = r.count(10);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        uint16_t idx = r.u16();
        uint64_t v = r.u64();
        if (idx >= kBuckets) return false;
        buckets_[idx] = v;
    }
    return r.ok();
}

void Metrics::encode(Writer& w) const {
    w.u32(uint32_t(counters_.size()));
    for (const auto& [k, v] : counters_) {
        w.str(k);
        w.u64(v);
    }
    w.u32(uint32_t(hists_.size()));
    for (const auto& [k, h] : hists_) {
        w.str(k);
        h.encode(w);
    }
}

bool Metrics::decode(Reader& r) {
    counters_.clear();
    hists_.clear();
    uint32_t n = r.count(12);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        std::string k = r.str();
        counters_[k] = r.u64();
    }
    n = r.count(40);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        std::string k = r.str();
        if (!hists_[k].decode(r)) return false;
    }
    return r.ok();
}

std::string format_ns(uint64_t ns) {
    if (ns < 1000) return format("%lluns", (unsigned long long)ns);
    if (ns < 1000000) return format("%.1fus", double(ns) / 1e3);
    if (ns < 1000000000ull) return format("%.2fms", double(ns) / 1e6);
    return format("%.2fs", double(ns) / 1e9);
}

std::string Metrics::render(const std::string& indent) const {
    std::string out;
    for (const auto& [k, v] : counters_)
        out += format("%s%-40s %s\n", indent.c_str(), k.c_str(), with_commas(v).c_str());
    for (const auto& [k, h] : hists_) {
        bool ns = k.size() > 3 && k.compare(k.size() - 3, 3, "_ns") == 0;
        auto f = [&](uint64_t v) { return ns ? format_ns(v) : with_commas(v); };
        out += format("%s%-40s n=%s min=%s p50=%s p90=%s p99=%s max=%s mean=%s\n", indent.c_str(),
                      k.c_str(), with_commas(h.count()).c_str(), f(h.min()).c_str(),
                      f(h.quantile(0.50)).c_str(), f(h.quantile(0.90)).c_str(),
                      f(h.quantile(0.99)).c_str(), f(h.max()).c_str(), f(uint64_t(h.mean())).c_str());
    }
    return out;
}

} // namespace dnuma
