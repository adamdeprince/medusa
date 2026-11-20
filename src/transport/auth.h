/* SPDX-License-Identifier: GPL-2.0-only */
// Mutual authentication for the management channel (docs/protocol.md).
//
// Both sides hold the pre-shared cluster key. The master sends a nonce; the
// agent answers with its own nonce and a proof bound to both nonces and its
// identity; the master answers with a proof bound to both nonces and its
// instance id. A session key derived from both nonces then authenticates
// every later frame. The key itself never crosses the wire.
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "common/sha256.h"

namespace dnuma {

namespace auth_detail {
inline Digest tagged(const std::vector<uint8_t>& key, const char* label,
                     std::initializer_list<std::pair<const void*, size_t>> parts) {
    std::vector<uint8_t> buf(label, label + strlen(label) + 1);
    for (const auto& [p, n] : parts) {
        uint64_t len = n; // length-prefix every part so fields cannot run together
        const uint8_t* lp = reinterpret_cast<const uint8_t*>(&len);
        buf.insert(buf.end(), lp, lp + sizeof len);
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    return hmac_sha256(key.data(), key.size(), buf.data(), buf.size());
}
} // namespace auth_detail

inline std::vector<uint8_t> agent_proof(const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce_m,
                                        const std::vector<uint8_t>& nonce_a, const std::string& uuid) {
    Digest d = auth_detail::tagged(key, "dnuma-agent-proof",
                                   {{nonce_m.data(), nonce_m.size()}, {nonce_a.data(), nonce_a.size()}, {uuid.data(), uuid.size()}});
    return {d.begin(), d.end()};
}

inline std::vector<uint8_t> master_proof(const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce_m,
                                         const std::vector<uint8_t>& nonce_a, uint64_t instance) {
    Digest d = auth_detail::tagged(key, "dnuma-master-proof",
                                   {{nonce_a.data(), nonce_a.size()}, {nonce_m.data(), nonce_m.size()}, {&instance, sizeof instance}});
    return {d.begin(), d.end()};
}

inline std::vector<uint8_t> session_key(const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce_m,
                                        const std::vector<uint8_t>& nonce_a) {
    Digest d = auth_detail::tagged(key, "dnuma-session-key",
                                   {{nonce_m.data(), nonce_m.size()}, {nonce_a.data(), nonce_a.size()}});
    return {d.begin(), d.end()};
}

inline bool proof_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a.size() == b.size() && !a.empty() && mac_equal(a.data(), b.data(), a.size());
}

} // namespace dnuma
