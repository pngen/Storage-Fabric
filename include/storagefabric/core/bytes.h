#pragma once
// Storage Fabric - byte containers, spans, bounded readers and hex helpers.
// All parsing is bounded: readers track remaining bytes and refuse to overrun.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <string>
#include <string_view>
#include <algorithm>
#include <cstring>
#include <optional>

#include "storagefabric/core/status.h"

namespace storagefabric {

using Bytes = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;
using MutableByteSpan = std::span<std::uint8_t>;

// Converts bytes to a lower-case hex string.
inline std::string to_hex(ByteSpan data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const auto b : data) {
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

// Decodes a hex string into bytes. Returns nullopt on invalid input.
inline std::optional<Bytes> from_hex(std::string_view s) {
    if ((s.size() % 2) != 0) return std::nullopt;
    Bytes out;
    out.reserve(s.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nibble(s[i]);
        int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

// Constant-access equality of byte spans.
inline bool bytes_equal(ByteSpan a, ByteSpan b) noexcept {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// A bounded byte reader over a span. Tracks position and remaining bytes.
class ByteReader {
public:
    explicit ByteReader(ByteSpan data) noexcept : data_(data) {}

    size_t position() const noexcept { return pos_; }
    size_t remaining() const noexcept { return data_.size() - pos_; }
    bool empty() const noexcept { return remaining() == 0; }
    ByteSpan whole() const noexcept { return data_; }
    ByteSpan rest() const noexcept { return data_.subspan(pos_); }

    bool read_u8(std::uint8_t& out) {
        if (remaining() < 1) return false;
        out = data_[pos_++];
        return true;
    }
    bool read_u16(std::uint16_t& out) {
        if (remaining() < 2) return false;
        out = static_cast<std::uint16_t>(data_[pos_]) |
              (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return true;
    }
    bool read_u32(std::uint32_t& out) {
        if (remaining() < 4) return false;
        out = static_cast<std::uint32_t>(data_[pos_]) |
              (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
              (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
              (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }
    bool read_u64(std::uint64_t& out) {
        if (remaining() < 8) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += 8;
        return true;
    }
    // Reads a length-prefixed byte blob where the length prefix is a u32.
    bool read_blob(ByteSpan& out) {
        std::uint32_t len = 0;
        if (!read_u32(len)) return false;
        if (remaining() < len) return false;
        out = data_.subspan(pos_, len);
        pos_ += len;
        return true;
    }
    bool read_bytes(size_t n, ByteSpan& out) {
        if (remaining() < n) return false;
        out = data_.subspan(pos_, n);
        pos_ += n;
        return true;
    }
    bool skip(size_t n) {
        if (remaining() < n) return false;
        pos_ += n;
        return true;
    }

private:
    ByteSpan data_;
    size_t pos_{0};
};

// A bounded byte writer that hands out fixed little-endian encoded fields.
class ByteWriter {
public:
    explicit ByteWriter(size_t reserve = 0) { buf_.reserve(reserve); }

    bool put_u8(std::uint8_t v) { buf_.push_back(v); return true; }
    bool put_u16(std::uint16_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        return true;
    }
    bool put_u32(std::uint32_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
        return true;
    }
    bool put_u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
        }
        return true;
    }
    bool put_blob(ByteSpan b) {
        put_u32(static_cast<std::uint32_t>(b.size()));
        buf_.insert(buf_.end(), b.begin(), b.end());
        return true;
    }
    bool put_bytes(ByteSpan b) { buf_.insert(buf_.end(), b.begin(), b.end()); return true; }

    const Bytes& bytes() const noexcept { return buf_; }
    Bytes take() { return std::move(buf_); }
    size_t size() const noexcept { return buf_.size(); }

private:
    Bytes buf_;
};

// Value used to signal "unbounded / unlimited" in bounded parsing limits.
inline constexpr std::size_t kUnbounded = static_cast<std::size_t>(-1);

}  // namespace storagefabric
