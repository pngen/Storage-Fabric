#include "test_util.h"
#include "storagefabric/model/manifest.h"

#include <vector>

using namespace storagefabric;

// Helper: a valid chunk descriptor covering [offset, offset+len).
static ChunkDescriptor chunk(std::uint64_t id, std::uint64_t offset,
                             std::uint64_t len, std::uint64_t blob) {
    ChunkDescriptor c;
    c.id = ChunkId(id);
    c.generation = ChunkGeneration(1);
    c.offset = offset;
    c.logical_length = len;
    c.physical_length = len;
    const Bytes b = {'b', static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(offset)};
    c.digest = ContentDigest::of(ByteSpan(b.data(), b.size()));
    c.blob = BlobId(blob);
    c.provenance = AuthorityOrigin::WORKER;
    return c;
}

static Manifest make_manifest(const std::vector<ChunkDescriptor>& chunks,
                              std::uint64_t total) {
    Manifest m;
    m.id = ManifestId(1);
    m.generation = ManifestGeneration(1);
    m.object = ObjectId(100);
    m.object_generation = ObjectGeneration(1);
    m.total_logical_length = total;
    const Bytes md = {'m', 'a', 'n', 'i'};
    m.manifest_digest = ContentDigest::of(ByteSpan(md.data(), md.size()));
    m.chunks = chunks;
    return m;
}

int main() {
    std::printf("test_manifest starting\n");

    // ---- valid manifest ----
    Manifest valid = make_manifest({chunk(1, 0, 60, 10), chunk(2, 60, 40, 20)}, 100);
    CHECK_STATUS(valid.validate());
    std::printf("  valid manifest PASS\n");

    // ---- gap ----
    Manifest gap = make_manifest({chunk(1, 0, 60, 10), chunk(2, 70, 30, 20)}, 100);
    auto g = validate_manifest(gap, false);
    CHECK(!g.ok);
    CHECK_EQ(static_cast<int>(g.code), static_cast<int>(StatusCode::Malformed));
    std::printf("  gap rejection PASS\n");

    // ---- overlap ----
    Manifest overlap = make_manifest({chunk(1, 0, 60, 10), chunk(2, 30, 70, 20)}, 100);
    auto o = validate_manifest(overlap, false);
    CHECK(!o.ok);
    CHECK_EQ(static_cast<int>(o.code), static_cast<int>(StatusCode::Malformed));
    std::printf("  overlap rejection PASS\n");

    // ---- duplicate chunk id ----
    Manifest dup = make_manifest({chunk(1, 0, 60, 10), chunk(1, 60, 40, 20)}, 100);
    auto d = validate_manifest(dup, false);
    CHECK(!d.ok);
    CHECK_EQ(static_cast<int>(d.code), static_cast<int>(StatusCode::DuplicateIdentity));
    std::printf("  duplicate chunk id rejection PASS\n");

    // ---- non-covering (cursor != total) ----
    Manifest short_cover = make_manifest({chunk(1, 0, 40, 10)}, 100);
    auto sc = validate_manifest(short_cover, false);
    CHECK(!sc.ok);
    CHECK_EQ(static_cast<int>(sc.code), static_cast<int>(StatusCode::LengthMismatch));
    std::printf("  non-covering rejection PASS\n");

    // ---- total-length mismatch (chunk extends beyond object) ----
    Manifest overrun = make_manifest({chunk(1, 0, 120, 10)}, 100);
    auto orr = validate_manifest(overrun, false);
    CHECK(!orr.ok);
    CHECK_EQ(static_cast<int>(orr.code), static_cast<int>(StatusCode::LengthMismatch));
    std::printf("  total-length mismatch rejection PASS\n");

    // ---- offset overflow ----
    Manifest off_over = make_manifest({chunk(1, 150, 10, 10)}, 100);
    auto oo = validate_manifest(off_over, false);
    CHECK(!oo.ok);
    CHECK_EQ(static_cast<int>(oo.code), static_cast<int>(StatusCode::Overflow));
    std::printf("  offset overflow rejection PASS\n");

    // ---- structural sanity ----
    Manifest no_chunks;
    no_chunks.id = ManifestId(1);
    no_chunks.object = ObjectId(1);
    no_chunks.total_logical_length = 10;
    const Bytes md2 = {'x'};
    no_chunks.manifest_digest = ContentDigest::of(ByteSpan(md2.data(), md2.size()));
    auto nc = validate_manifest(no_chunks, false);
    CHECK(!nc.ok);
    std::printf("  empty-chunk manifest rejected PASS\n");

    // ---- deterministic chunk sort ----
    Manifest unsorted = make_manifest({chunk(2, 60, 40, 20), chunk(1, 0, 60, 10)}, 100);
    // Out of order by offset; validate() would reject non-deterministic ordering.
    auto pre = validate_manifest(unsorted, false);
    CHECK(!pre.ok);
    unsorted.sort_chunks();
    CHECK(unsorted.chunks[0].offset == 0u);
    CHECK(unsorted.chunks[1].offset == 60u);
    CHECK_STATUS(unsorted.validate());
    std::printf("  deterministic chunk sort PASS\n");

    std::printf("test_manifest: ALL PASS\n");
    return 0;
}
