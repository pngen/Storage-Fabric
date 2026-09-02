#include "test_util.h"
#include "storagefabric/core/protocol.h"
#include "storagefabric/core/bytes.h"

#include <vector>
#include <cstring>

using namespace storagefabric;

static Bytes make_frame_bytes(const Bytes& payload, std::uint16_t kind_raw) {
    // Builds a frame header with the given raw kind and a correct CRC.
    ByteWriter w;
    w.put_u32(kFrameMagic);
    w.put_u16(kProtocolVersion);
    w.put_u16(kind_raw);
    w.put_u32(static_cast<std::uint32_t>(payload.size()));
    w.put_u32(0);
    Bytes header = w.bytes();
    const std::uint32_t crc = frame_crc(ByteSpan(header.data(), header.size()),
                                        ByteSpan(payload.data(), payload.size()));
    header[12] = static_cast<std::uint8_t>(crc & 0xFF);
    header[13] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    header[14] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
    header[15] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);
    Bytes out = header;
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

int main() {
    std::printf("test_protocol starting\n");

    // ---- round-trip across every wire kind ----
    const Bytes payload = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    const std::vector<WireMessageKind> kinds = {
        WireMessageKind::HELLO, WireMessageKind::REGISTER_BACKEND,
        WireMessageKind::CREATE_OBJECT, WireMessageKind::PUBLISH,
        WireMessageKind::READ_OBJECT, WireMessageKind::VERIFY,
        WireMessageKind::COMMIT, WireMessageKind::REPLICATE,
        WireMessageKind::RECOVER, WireMessageKind::PING, WireMessageKind::PONG,
        WireMessageKind::ERROR, WireMessageKind::GOODBYE};
    for (const WireMessageKind k : kinds) {
        const Bytes frame = encode_frame(k, ByteSpan(payload.data(), payload.size()));
        CHECK(frame.size() == kFrameHeaderSize + payload.size());
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(frame.data(), frame.size()), consumed);
        CHECK(dec.ok());
        CHECK(dec.value().kind == k);
        CHECK(bytes_eq(dec.value().payload, payload));
        CHECK(consumed == frame.size());
        CHECK(dec.value().crc != 0u);
    }
    std::printf("  round-trip across wire kinds PASS\n");

    // ---- reject bad magic ----
    {
        const Bytes frame = encode_frame(WireMessageKind::PING, ByteSpan(payload.data(), payload.size()));
        Bytes bad = frame;
        bad[0] ^= 0xFF;
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(bad.data(), bad.size()), consumed);
        CHECK(!dec.ok());
        CHECK_EQ(static_cast<int>(dec.error_code()), static_cast<int>(StatusCode::ProtocolError));
    }
    // ---- reject unsupported version ----
    {
        const Bytes frame = encode_frame(WireMessageKind::PING, ByteSpan(payload.data(), payload.size()));
        Bytes bad = frame;
        bad[4] = 0xFF;
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(bad.data(), bad.size()), consumed);
        CHECK(!dec.ok());
        CHECK_EQ(static_cast<int>(dec.error_code()), static_cast<int>(StatusCode::ProtocolError));
    }
    // ---- reject oversize payload (declared length > max) ----
    {
        const Bytes frame = encode_frame(WireMessageKind::PING, ByteSpan(payload.data(), payload.size()));
        Bytes over = frame;
        over[8] = 0xFF; over[9] = 0xFF; over[10] = 0xFF; over[11] = 0xFF;  // length = 0xFFFFFFFF
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(over.data(), over.size()), consumed);
        CHECK(!dec.ok());
        CHECK_EQ(static_cast<int>(dec.error_code()), static_cast<int>(StatusCode::Overflow));
    }
    // ---- reject truncation ----
    {
        const Bytes frame = encode_frame(WireMessageKind::PING, ByteSpan(payload.data(), payload.size()));
        Bytes short_frame(frame.begin(), frame.begin() + kFrameHeaderSize);  // header only, no payload
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(short_frame.data(), short_frame.size()), consumed);
        CHECK(!dec.ok());
        CHECK_EQ(static_cast<int>(dec.error_code()), static_cast<int>(StatusCode::Truncated));
    }
    // ---- reject checksum mismatch ----
    {
        const Bytes frame = encode_frame(WireMessageKind::PING, ByteSpan(payload.data(), payload.size()));
        Bytes corrupt = frame;
        corrupt[corrupt.size() - 1] ^= 0x01;   // flip a payload byte
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(corrupt.data(), corrupt.size()), consumed);
        CHECK(!dec.ok());
        CHECK_EQ(static_cast<int>(dec.error_code()), static_cast<int>(StatusCode::IntegrityMismatch));
    }
    // ---- reject invalid enum (valid CRC, bad kind) ----
    {
        const Bytes frame = make_frame_bytes(payload, 0x3E7);   // 999, not a valid kind
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(frame.data(), frame.size()), consumed);
        CHECK(!dec.ok());
        CHECK_EQ(static_cast<int>(dec.error_code()), static_cast<int>(StatusCode::ProtocolError));
    }
    std::printf("  bad magic/version/oversize/truncation/checksum/enum rejection PASS\n");

    // ---- trailing bytes: caller observes leftover via consumed ----
    {
        const Bytes frame = encode_frame(WireMessageKind::PING, ByteSpan(payload.data(), payload.size()));
        Bytes with_tail = frame;
        with_tail.push_back(0x00);
        with_tail.push_back(0xFF);
        std::size_t consumed = 0;
        const auto dec = decode_frame(ByteSpan(with_tail.data(), with_tail.size()), consumed);
        CHECK(dec.ok());                       // decoder accepts a leading frame...
        CHECK(consumed == frame.size());       // ...and reports how many bytes it used
        CHECK(consumed < with_tail.size());    // the caller sees the trailing bytes
        std::printf("  caller-detected trailing bytes via consumed PASS\n");
    }

    // ---- check_frame_for_body ----
    {
        const Bytes fb = encode_frame(WireMessageKind::PING, ByteSpan(payload.data(), payload.size()));
        std::size_t consumed = 0;
        auto dec = decode_frame(ByteSpan(fb.data(), fb.size()), consumed);
        CHECK(dec.ok());
        CHECK_STATUS(check_frame_for_body(dec.value()));
        std::printf("  check_frame_for_body PASS\n");
    }

    std::printf("test_protocol: ALL PASS\n");
    return 0;
}
