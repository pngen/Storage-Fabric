#include "test_util.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/bytes.h"

#include <string>

using namespace storagefabric;

// Span a null-terminated string literal as a byte span.
static ByteSpan str_span(const char* s) {
    return ByteSpan(reinterpret_cast<const std::uint8_t*>(s), std::char_traits<char>::length(s));
}

int main() {
    std::printf("test_integrity starting\n");

    // ---- SHA-256 known vectors ----
    // "hello world"
    const std::string hello_hex = sha256_hex(str_span("hello world"));
    CHECK_EQ(hello_hex, std::string("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"));
    // empty input
    const std::string empty_hex = sha256_hex(str_span(""));
    CHECK_EQ(empty_hex, std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    std::printf("  SHA-256 known vectors PASS\n");

    // ---- CRC-32 known vector ----
    // The canonical CRC-32 check value for the ASCII string "123456789".
    const std::uint32_t crc_check = crc32(str_span("123456789"));
    CHECK_EQ(crc_check, 0xCBF43926u);
    CHECK_EQ(crc32(str_span("")), 0x00000000u);
    std::printf("  CRC-32 known vector PASS\n");

    // ---- truncation detection ----
    const Bytes full = {'t', 'h', 'e', ' ', 'q', 'u', 'i', 'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n'};
    const Bytes truncated(full.begin(), full.begin() + 5);
    const ContentDigest dfull = ContentDigest::of(ByteSpan(full.data(), full.size()));
    const ContentDigest dtrunc = ContentDigest::of(ByteSpan(truncated.data(), truncated.size()));
    CHECK(!(dfull == dtrunc));   // a truncation changes the digest
    CHECK(dfull.hex().size() == 64u);
    std::printf("  truncation detection PASS\n");

    // ---- digest mismatch detection ----
    const ContentDigest expected = ContentDigest::of(ByteSpan(full.data(), full.size()));
    const ContentDigest wrong = ContentDigest::of(ByteSpan(truncated.data(), truncated.size()));
    CHECK(expected != wrong);
    // An integrity check comparing recomputed vs expected must detect the mismatch.
    const ContentDigest recomputed = ContentDigest::of(ByteSpan(full.data(), full.size()));
    CHECK(recomputed == expected);
    CHECK(!(recomputed == wrong));
    std::printf("  digest mismatch detection PASS\n");

    // ---- ContentDigest of / hex ----
    const std::string cstr = "abc";
    const ContentDigest cd = ContentDigest::of(str_span("abc"));
    CHECK_EQ(cd.hex(), sha256_hex(str_span("abc")));
    CHECK_EQ(cd.hex(), to_hex(ByteSpan(cd.bytes.data(), cd.bytes.size())));
    CHECK(!cd.is_zero());
    ContentDigest zero;
    CHECK(zero.is_zero());
    CHECK(cd.short_hex(8).size() == 8u);
    // Same content yields the same digest; different content differs.
    CHECK(ContentDigest::of(str_span("abc")) == ContentDigest::of(str_span("abc")));
    CHECK(ContentDigest::of(str_span("abc")) != ContentDigest::of(str_span("abd")));
    std::printf("  ContentDigest of/hex PASS\n");

    // ---- incremental SHA-256 equals one-shot ----
    Sha256 inc;
    for (const char ch : std::string("hello world")) {
        const std::uint8_t b = static_cast<std::uint8_t>(ch);
        inc.update(ByteSpan(&b, 1));
    }
    CHECK(bytes_eq(inc.finish(), sha256(str_span("hello world"))));
    // Sha256 object is reusable after finish().
    Sha256 h2;
    h2.update(str_span("hello world"));
    CHECK(bytes_eq(h2.digest(), sha256(str_span("hello world"))));
    std::printf("  incremental SHA-256 PASS\n");

    std::printf("test_integrity: ALL PASS\n");
    return 0;
}
