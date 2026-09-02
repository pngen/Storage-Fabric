#include "storagefabric/core/digest.h"

#include <cstring>

namespace storagefabric {

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3) reflected table driven.
// ---------------------------------------------------------------------------
namespace {
struct Crc32Table {
    std::array<std::uint32_t, 256> t{};
    Crc32Table() noexcept {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
    }
};
const Crc32Table& crc_table() {
    static const Crc32Table table;
    return table;
}
}  // namespace

std::uint32_t crc32(ByteSpan data) noexcept {
    std::uint32_t c = 0xFFFFFFFFu;
    const auto& table = crc_table();
    for (const auto b : data) {
        c = table.t[(c ^ b) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

namespace {
std::uint32_t gf2_matrix_times(const std::uint32_t* mat, std::uint32_t vec) noexcept {
    std::uint32_t sum = 0;
    while (vec) {
        if (vec & 1) sum ^= *mat;
        vec >>= 1;
        ++mat;
    }
    return sum;
}
void gf2_matrix_square(std::uint32_t* square, const std::uint32_t* mat) noexcept {
    for (int n = 0; n < 32; ++n) square[n] = gf2_matrix_times(mat, mat[n]);
}
}  // namespace

std::uint32_t crc32_combine(std::uint32_t crc1, std::uint32_t crc2, size_t len2) noexcept {
    // Combines two CRC-32 values so that the result equals the CRC of the
    // concatenation of the two original byte streams. len2 is the length of
    // the second stream (the one that produced crc2).
    if (len2 == 0) return crc1;
    std::uint32_t even[32];
    std::uint32_t odd[32];
    odd[0] = 0xEDB88320u;
    std::uint32_t row = 1;
    for (int n = 1; n < 32; ++n) {
        odd[n] = row;
        row <<= 1;
    }
    gf2_matrix_square(even, odd);
    gf2_matrix_square(odd, even);
    std::uint64_t len = static_cast<std::uint64_t>(len2);
    do {
        gf2_matrix_square(even, odd);
        if (len & 1) crc1 = gf2_matrix_times(even, crc1);
        len >>= 1;
        if (len == 0) break;
        gf2_matrix_square(odd, even);
        if (len & 1) crc1 = gf2_matrix_times(odd, crc1);
        len >>= 1;
    } while (len != 0);
    return crc1 ^ crc2;
}

// ---------------------------------------------------------------------------
// SHA-256.
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint32_t kK[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
constexpr std::uint32_t kH[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(std::uint32_t* state, const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
               (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(p[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t t1 = h + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}
}  // namespace

Sha256::Sha256() {
    std::memcpy(state_, kH, sizeof(state_));
}

void Sha256::process_block(const std::uint8_t* p) {
    compress(state_, p);
    bit_count_ += 512;
}

void Sha256::update(ByteSpan data) {
    for (const auto b : data) {
        buffer_[buffer_len_++] = b;
        if (buffer_len_ == 64) {
            process_block(buffer_.data());
            buffer_len_ = 0;
        }
    }
}

Bytes Sha256::finish() {
    const std::uint64_t bit_len = bit_count_ + static_cast<std::uint64_t>(buffer_len_) * 8;
    const std::uint8_t pad = 0x80;
    update(ByteSpan(&pad, 1));
    std::uint8_t zero = 0x00;
    while (buffer_len_ != 56) {
        update(ByteSpan(&zero, 1));
    }
    // append 64-bit big-endian bit length
    std::uint8_t lenbuf[8];
    for (int i = 0; i < 8; ++i) {
        lenbuf[i] = static_cast<std::uint8_t>((bit_len >> (56 - 8 * i)) & 0xFF);
    }
    update(ByteSpan(lenbuf, 8));
    Bytes out;
    out.reserve(32);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((state_[i] >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((state_[i] >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((state_[i] >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(state_[i] & 0xFF));
    }
    // Reset state so the object may be reused.
    std::memcpy(state_, kH, sizeof(state_));
    buffer_len_ = 0;
    bit_count_ = 0;
    return out;
}

Bytes Sha256::digest() const {
    Sha256 copy = *this;
    return copy.finish();
}

Bytes sha256(ByteSpan data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

std::string sha256_hex(ByteSpan data) {
    return to_hex(ByteSpan(sha256(data).data(), 32));
}

void ContentDigest::set_from(ByteSpan data) {
    Bytes d = sha256(data);
    std::memcpy(bytes.data(), d.data(), 32);
}

ContentDigest ContentDigest::of(ByteSpan data) {
    ContentDigest c;
    c.set_from(data);
    return c;
}

bool ContentDigest::is_zero() const noexcept {
    for (const auto b : bytes) {
        if (b != 0) return false;
    }
    return true;
}

std::string ContentDigest::short_hex(size_t n) const {
    const std::string h = hex();
    return h.substr(0, std::min(n, h.size()));
}

}  // namespace storagefabric
