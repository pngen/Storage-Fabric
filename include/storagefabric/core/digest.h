#pragma once
// Storage Fabric - content integrity primitives.
// SHA-256 is the primary content identity digest. CRC-32 is used for framing
// and persistence integrity, never as the sole content identity of a large
// storage object.

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>

#include "storagefabric/core/bytes.h"

namespace storagefabric {

// FNV-1a 64 hash for internal table keys (not a content digest).
inline std::uint64_t fnv1a64(ByteSpan data) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto b : data) {
        h ^= b;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
std::uint32_t crc32(ByteSpan data) noexcept;
std::uint32_t crc32_combine(std::uint32_t a, std::uint32_t b, size_t b_len) noexcept;

// One-shot SHA-256 (returns 32 bytes) and hex forms.
Bytes sha256(ByteSpan data);
std::string sha256_hex(ByteSpan data);

// Incremental SHA-256 for streaming verification.
class Sha256 {
public:
    Sha256();
    void update(ByteSpan data);
    Bytes finish();  // finalizes and returns the 32-byte digest
    Bytes digest() const;  // finalizes a copy, leaves the object usable
    static constexpr size_t kDigestSize = 32;

private:
    void process_block(const std::uint8_t* p);
    std::uint32_t state_[8];
    std::uint64_t bit_count_{0};
    std::array<std::uint8_t, 64> buffer_{};
    size_t buffer_len_{0};
};

// A 32-byte content digest value.
struct ContentDigest {
    std::array<std::uint8_t, 32> bytes{};

    bool operator==(const ContentDigest& o) const noexcept = default;
    auto operator<=>(const ContentDigest& o) const noexcept = default;

    void set_from(ByteSpan data);
    std::string hex() const { return to_hex(ByteSpan(bytes.data(), bytes.size())); }
    bool is_zero() const noexcept;
    static ContentDigest of(ByteSpan data);

    // Stable short prefix for display.
    std::string short_hex(size_t n = 12) const;
};

// Equality helpers for content digests.
inline bool digest_equal(ByteSpan a, ByteSpan b) noexcept { return bytes_equal(a, b); }

}  // namespace storagefabric

namespace std {
template <>
struct hash<::storagefabric::ContentDigest> {
    size_t operator()(const ::storagefabric::ContentDigest& d) const noexcept {
        // Fold the leading 8 bytes; content digests are keyed by this cheap
        // stable value and compared in full on equality.
        return static_cast<size_t>(::storagefabric::fnv1a64(::storagefabric::ByteSpan(d.bytes.data(), d.bytes.size())));
    }
};
}  // namespace std
