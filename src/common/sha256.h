/* SPDX-License-Identifier: GPL-2.0-only */
// SHA-256 and HMAC-SHA-256 (FIPS 180-4 / RFC 2104), implemented in-tree so the
// control plane has no external crypto dependency. Verified against the
// standard test vectors in tests/unit.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"

namespace dnuma {

using Digest = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    Digest finish();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

Digest sha256(const void* data, size_t len);
Digest hmac_sha256(const void* key, size_t key_len, const void* data, size_t len);
Digest hmac_sha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data);
Status sha256_file(const std::string& path, Digest* out);
// Constant-time comparison of two MACs.
bool mac_equal(const uint8_t* a, const uint8_t* b, size_t n);

} // namespace dnuma
