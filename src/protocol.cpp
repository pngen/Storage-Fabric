#include "storagefabric/core/protocol.h"

#include <cstring>

namespace storagefabric {

const char* to_string(WireMessageKind v) noexcept {
    switch (v) {
        case WireMessageKind::HELLO: return "HELLO";
        case WireMessageKind::REGISTER_BACKEND: return "REGISTER_BACKEND";
        case WireMessageKind::CREATE_OBJECT: return "CREATE_OBJECT";
        case WireMessageKind::PUBLISH: return "PUBLISH";
        case WireMessageKind::READ_OBJECT: return "READ_OBJECT";
        case WireMessageKind::VERIFY: return "VERIFY";
        case WireMessageKind::COMMIT: return "COMMIT";
        case WireMessageKind::REPLICATE: return "REPLICATE";
        case WireMessageKind::RECOVER: return "RECOVER";
        case WireMessageKind::PING: return "PING";
        case WireMessageKind::PONG: return "PONG";
        case WireMessageKind::ERROR: return "ERROR";
        case WireMessageKind::GOODBYE: return "GOODBYE";
    }
    return "UNKNOWN";
}

Bytes encode_frame(WireMessageKind kind, ByteSpan payload) {
    if (payload.size() > kMaxFramePayload) {
        return Bytes();   // caller must bound payload; empty encodes can't happen here
    }
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    ByteWriter w;
    w.put_u32(kFrameMagic);
    w.put_u16(kProtocolVersion);
    w.put_u16(static_cast<std::uint16_t>(kind));
    w.put_u32(length);
    w.put_u32(0);   // crc placeholder
    // Compute CRC over header (first 12 bytes) + payload.
    Bytes header = w.bytes();
    const std::uint32_t crc = crc32_combine(crc32(ByteSpan(header.data(), 12)), crc32(payload), payload.size());
    // Patch crc field (bytes 12..15 little-endian).
    header[12] = static_cast<std::uint8_t>(crc & 0xFF);
    header[13] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    header[14] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
    header[15] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);
    Bytes out;
    out.reserve(kFrameHeaderSize + payload.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::uint32_t frame_crc(ByteSpan header_bytes, ByteSpan payload) noexcept {
    // header_bytes must be 16 bytes (magic..crc). CRC covers bytes 0..12 + payload.
    return crc32_combine(crc32(header_bytes.first(12)), crc32(payload), payload.size());
}

Result<Frame> decode_frame(ByteSpan data, std::size_t& consumed) {
    consumed = 0;
    if (data.size() < kFrameHeaderSize) {
        return Result<Frame>::failure(StatusCode::Truncated, "frame shorter than header");
    }
    ByteReader r(data);
    std::uint32_t magic = 0, length = 0;
    std::uint16_t version = 0, kind_raw = 0;
    std::uint32_t stored_crc = 0;
    if (!r.read_u32(magic) || !r.read_u16(version) || !r.read_u16(kind_raw) ||
        !r.read_u32(length) || !r.read_u32(stored_crc)) {
        return Result<Frame>::failure(StatusCode::Truncated, "truncated header");
    }
    if (magic != kFrameMagic) return Result<Frame>::failure(StatusCode::ProtocolError, "bad magic");
    if (version != kProtocolVersion) return Result<Frame>::failure(StatusCode::ProtocolError, "unsupported version");
    if (length > kMaxFramePayload) return Result<Frame>::failure(StatusCode::Overflow, "oversized payload");
    if (data.size() < kFrameHeaderSize + static_cast<std::size_t>(length)) {
        return Result<Frame>::failure(StatusCode::Truncated, "truncated payload");
    }
    const ByteSpan payload = data.subspan(kFrameHeaderSize, length);
    // Verify CRC over header(12) + payload.
    const std::uint32_t recomputed = frame_crc(data.subspan(0, kFrameHeaderSize), payload);
    if (recomputed != stored_crc) {
        return Result<Frame>::failure(StatusCode::IntegrityMismatch, "checksum mismatch");
    }
    // Bound kind validity.
    switch (static_cast<WireMessageKind>(kind_raw)) {
        case WireMessageKind::HELLO: case WireMessageKind::REGISTER_BACKEND:
        case WireMessageKind::CREATE_OBJECT: case WireMessageKind::PUBLISH:
        case WireMessageKind::READ_OBJECT: case WireMessageKind::VERIFY:
        case WireMessageKind::COMMIT: case WireMessageKind::REPLICATE:
        case WireMessageKind::RECOVER: case WireMessageKind::PING:
        case WireMessageKind::PONG: case WireMessageKind::ERROR:
        case WireMessageKind::GOODBYE: break;
        default:
            return Result<Frame>::failure(StatusCode::ProtocolError, "invalid enum");
    }
    Frame f;
    f.kind = static_cast<WireMessageKind>(kind_raw);
    f.payload = Bytes(payload.begin(), payload.end());
    f.crc = stored_crc;
    consumed = kFrameHeaderSize + static_cast<std::size_t>(length);
    return f;
}

Status check_frame_for_body(const Frame& f) {
    if (f.payload.size() > kMaxFramePayload) {
        return Status(StatusCode::Overflow, "frame payload exceeds max");
    }
    return Status::ok_status();
}

}  // namespace storagefabric
