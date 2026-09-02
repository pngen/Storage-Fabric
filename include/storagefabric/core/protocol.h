#pragma once
// Storage Fabric - versioned, bounded framed wire protocol.
// Frame layout (little-endian):
//   magic(4) | version(2) | kind(2) | payload_length(4) | crc32(4) | payload
// Decoding rejects bad magic, unsupported version, oversized payload,
// truncation, checksum mismatch, invalid enum, and trailing garbage.

#include <cstdint>
#include <string>
#include <vector>

#include "storagefabric/core/bytes.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/status.h"

namespace storagefabric {

constexpr std::uint32_t kFrameMagic = 0x53464231u;    // "SFB1"
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint32_t kMaxFramePayload = 64u * 1024 * 1024;
constexpr std::size_t kFrameHeaderSize = 16;

enum class WireMessageKind : std::uint16_t {
    HELLO = 1,
    REGISTER_BACKEND = 2,
    CREATE_OBJECT = 3,
    PUBLISH = 4,
    READ_OBJECT = 5,
    VERIFY = 6,
    COMMIT = 7,
    REPLICATE = 8,
    RECOVER = 9,
    PING = 10,
    PONG = 11,
    ERROR = 12,
    GOODBYE = 13,
};

inline const char* to_string(WireMessageKind) noexcept;

struct Frame {
    WireMessageKind kind{WireMessageKind::PING};
    Bytes payload;
    std::uint32_t crc{0};
};

// Encodes a complete frame (header + payload) with CRC over header+payload.
Bytes encode_frame(WireMessageKind kind, ByteSpan payload);

// Decodes a frame from the front of 'data'. On success 'consumed' is the number
// of bytes read. Rejects trailing garbage at the frame level when the whole
// buffer is not exactly one frame (callers may supply multiple frames).
Result<Frame> decode_frame(ByteSpan data, std::size_t& consumed);

// Validates that payload/header round-trip; returns the recomputed CRC.
std::uint32_t frame_crc(ByteSpan header_bytes, ByteSpan payload) noexcept;

// Bounded message body helpers used by the coordinator/worker RPC.
Status check_frame_for_body(const Frame& f);

}  // namespace storagefabric
