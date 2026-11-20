/* SPDX-License-Identifier: GPL-2.0-only */
// Counters and latency histograms. Histograms keep a distribution (log2
// buckets with 4 sub-buckets each), not just an average, and are mergeable so
// agents can ship snapshots to the master.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace dnuma {

class Histogram {
public:
    // 64 powers of two x 4 linear sub-buckets covers the full uint64 range.
    static constexpr size_t kBuckets = 64 * 4;

    void record(uint64_t v);
    void merge(const Histogram& o);
    uint64_t count() const { return count_; }
    uint64_t sum() const { return sum_; }
    uint64_t min() const { return count_ ? min_ : 0; }
    uint64_t max() const { return max_; }
    double mean() const { return count_ ? double(sum_) / double(count_) : 0.0; }
    // Upper bound of the bucket containing quantile q in [0,1].
    uint64_t quantile(double q) const;

    void encode(Writer& w) const;
    bool decode(Reader& r);

    static size_t bucket_of(uint64_t v);
    static uint64_t bucket_upper(size_t b);

private:
    std::array<uint64_t, kBuckets> buckets_{};
    uint64_t count_ = 0, sum_ = 0, min_ = UINT64_MAX, max_ = 0;
};

// A named set of counters and histograms. Not thread-safe: each daemon
// updates its registry from the reactor thread only.
class Metrics {
public:
    void add(const std::string& name, uint64_t delta = 1) { counters_[name] += delta; }
    void set(const std::string& name, uint64_t value) { counters_[name] = value; }
    uint64_t get(const std::string& name) const {
        auto it = counters_.find(name);
        return it == counters_.end() ? 0 : it->second;
    }
    void observe(const std::string& name, uint64_t value) { hists_[name].record(value); }
    const Histogram* hist(const std::string& name) const {
        auto it = hists_.find(name);
        return it == hists_.end() ? nullptr : &it->second;
    }

    const std::map<std::string, uint64_t>& counters() const { return counters_; }
    const std::map<std::string, Histogram>& hists() const { return hists_; }

    void merge(const Metrics& o) {
        for (const auto& [k, v] : o.counters_) counters_[k] += v;
        for (const auto& [k, h] : o.hists_) hists_[k].merge(h);
    }

    void encode(Writer& w) const;
    bool decode(Reader& r);
    // Human-readable dump; durations are assumed to be nanoseconds when the
    // metric name ends in "_ns".
    std::string render(const std::string& indent = "") const;

private:
    std::map<std::string, uint64_t> counters_;
    std::map<std::string, Histogram> hists_;
};

std::string format_ns(uint64_t ns);

} // namespace dnuma
