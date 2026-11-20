// SPDX-License-Identifier: GPL-2.0-only
#include <cstring>

#include "common/metrics.h"
#include "common/sha256.h"
#include "common/util.h"
#include "utest.h"

using namespace dnuma;

static std::string hexd(const Digest& d) { return hex(d.data(), d.size()); }

TEST(sha256_vectors) { // FIPS 180-4 examples
    CHECK(hexd(sha256("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hexd(sha256("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hexd(sha256(two, strlen(two))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 s; // one million 'a', fed in uneven pieces
    std::string chunk(977, 'a');
    size_t left = 1000000;
    while (left) {
        size_t n = std::min(left, chunk.size());
        s.update(chunk.data(), n);
        left -= n;
    }
    CHECK(hexd(s.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(hmac_vectors) { // RFC 4231 test cases 1, 2 and 6
    std::vector<uint8_t> k1(20, 0x0b);
    CHECK(hexd(hmac_sha256(k1.data(), k1.size(), "Hi There", 8)) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    const char* d2 = "what do ya want for nothing?";
    CHECK(hexd(hmac_sha256("Jefe", 4, d2, strlen(d2))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    std::vector<uint8_t> k6(131, 0xaa); // key longer than the block size
    const char* d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK(hexd(hmac_sha256(k6.data(), k6.size(), d6, strlen(d6))) ==
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(mac_compare) {
    uint8_t a[16] = {1, 2, 3}, b[16] = {1, 2, 3};
    CHECK(mac_equal(a, b, 16));
    b[15] = 1;
    CHECK(!mac_equal(a, b, 16));
}

TEST(parsers) {
    uint64_t v = 0;
    CHECK(parse_size("4096", &v) && v == 4096);
    CHECK(parse_size("64M", &v) && v == 64ull << 20);
    CHECK(parse_size("2G", &v) && v == 2ull << 30);
    CHECK(!parse_size("", &v) && !parse_size("12Q", &v) && !parse_size("abc", &v));
    CHECK(!parse_size("99999999999999999999T", &v)); // overflow is rejected, not wrapped
    CHECK(parse_duration_ms("300s", &v) && v == 300000);
    CHECK(parse_duration_ms("250ms", &v) && v == 250);
    CHECK(parse_duration_ms("5m", &v) && v == 300000);
    CHECK(parse_duration_ms("7", &v) && v == 7000);
    CHECK(!parse_duration_ms("5x", &v));
    std::vector<int> cpus = parse_cpulist("0-3,8,10-11\n");
    CHECK_EQ(cpus.size(), size_t(7));
    CHECK(format_cpulist(cpus) == "0-3,8,10-11");
    CHECK(format_cpulist(parse_cpulist("0,4,8,12")) == "0,4,8,12");
    CHECK(with_commas(183044) == "183,044" && with_commas(7) == "7" && with_commas(1000) == "1,000");
    CHECK(human_bytes(715ull << 20) == "715 MiB");
    std::vector<uint8_t> raw;
    CHECK(unhex("00ff10", &raw) && raw.size() == 3 && raw[1] == 0xff);
    CHECK(!unhex("0g", &raw) && !unhex("abc", &raw));
    CHECK(make_uuid().size() == 36 && make_uuid() != make_uuid());
}

TEST(histogram_distribution) {
    Histogram h;
    for (uint64_t i = 1; i <= 1000; ++i) h.record(i * 1000); // 1us .. 1ms
    CHECK_EQ(h.count(), 1000u);
    CHECK_EQ(h.min(), 1000u);
    CHECK_EQ(h.max(), 1000000u);
    // Buckets are 4 per power of two, so quantiles are exact to within 25%.
    uint64_t p50 = h.quantile(0.5), p99 = h.quantile(0.99);
    CHECK(p50 >= 500000 && p50 <= 625000 + 1000);
    CHECK(p99 >= 990000 && p99 <= 1000000);
    CHECK(h.quantile(0.0) >= 1000 && h.quantile(1.0) == 1000000);
    // Bucket boundaries: every value falls in a bucket whose upper bound is >= it.
    for (uint64_t v : {0ull, 1ull, 3ull, 4ull, 5ull, 1023ull, 1024ull, 1ull << 40, ~0ull})
        CHECK(Histogram::bucket_upper(Histogram::bucket_of(v)) >= v);
    Histogram other;
    other.record(5);
    h.merge(other);
    CHECK_EQ(h.count(), 1001u);
    CHECK_EQ(h.min(), 5u);
}

TEST(metrics_roundtrip) {
    Metrics m;
    m.add("faults", 3);
    m.add("faults");
    m.observe("lat_ns", 100);
    m.observe("lat_ns", 900);
    Writer w;
    m.encode(w);
    Metrics back;
    Reader r(w.data());
    CHECK(back.decode(r) && r.done());
    CHECK_EQ(back.get("faults"), 4u);
    CHECK(back.hist("lat_ns") && back.hist("lat_ns")->count() == 2 && back.hist("lat_ns")->max() == 900);
    CHECK(back.render().find("lat_ns") != std::string::npos);
}

UTEST_MAIN()
