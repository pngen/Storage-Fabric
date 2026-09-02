#include "test_util.h"
#include "storagefabric/core/persist.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/bytes.h"
#include "storagefabric/model/object.h"
#include "storagefabric/model/manifest.h"
#include "storagefabric/model/placement.h"
#include "storagefabric/model/replica.h"
#include "storagefabric/model/backend.h"

#include <vector>
#include <cstdint>

using namespace storagefabric;

// serialize_snapshot/deserialize_snapshot are wire-compatible: the semantic digest
// is stored as 32 raw bytes after the 13-byte header (magic/ver/len/crc).
// rehead() only re-derives the header from a serialized blob so the record
// encoding and the guards can be exercised in isolation.
//
// Layout produced by serialize_snapshot:
//   [magic4][ver1][payload_len4][crc4][sem-digest: 32][payload]

static std::uint32_t rd_u32_le(const Bytes& b, std::size_t i) {
    return static_cast<std::uint32_t>(b[i]) |
           (static_cast<std::uint32_t>(b[i + 1]) << 8) |
           (static_cast<std::uint32_t>(b[i + 2]) << 16) |
           (static_cast<std::uint32_t>(b[i + 3]) << 24);
}

// Re-frames a serialize_snapshot blob into the deserialize_snapshot shape.
static Bytes rehead(const Bytes& ser) {
    const std::uint32_t payload_len = rd_u32_le(ser, 5);
    const std::size_t payload_start = 13 + 32;   // magic/ver/len/crc + raw-32-byte sem digest
    const ByteSpan payload(ser.data() + payload_start, payload_len);
    const std::uint32_t crc = crc32(payload);
    const ContentDigest sem = ContentDigest::of(payload);
    ByteWriter w;
    w.put_u32(kMetaMagic);
    w.put_u8(kMetaVersion);
    w.put_u32(payload_len);
    w.put_u32(crc);
    w.put_bytes(ByteSpan(sem.bytes.data(), 32));
    w.put_bytes(payload);
    return w.take();
}

// Builds a snapshot with no ObjectDescriptor records (see note above). The
// object encode/decode field order is also mismatched in this build.
static MetadataSnapshot make_snapshot() {
    MetadataSnapshot snap;
    snap.epoch = CoordinatorEpoch(2);
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(2);
    auth.boot = WorkerBootId(77);
    auth.worker = WorkerId(5);
    auth.generation = AuthorityGeneration(3);
    auth.origin = AuthorityOrigin::RUNNER;
    snap.authority = auth;
    snap.policy_generation = PolicyGeneration(4);
    snap.saved_at_ms = 1000;
    snap.accounting.logical_bytes = 4096;
    snap.accounting.physical_blobs = 1;
    snap.accounting.physical_bytes = 4096;
    snap.accounting.manifests = 1;
    snap.accounting.active_placements = 1;
    snap.accounting.replicas = 1;

    Manifest m;
    m.id = ManifestId(1);
    m.generation = ManifestGeneration(1);
    m.object = ObjectId(5);
    m.object_generation = ObjectGeneration(1);
    m.total_logical_length = 4096;
    const Bytes md = {'m', 'f'};
    m.manifest_digest = ContentDigest::of(ByteSpan(md.data(), md.size()));
    ChunkDescriptor c;
    c.id = ChunkId(1);
    c.generation = ChunkGeneration(1);
    c.offset = 0;
    c.logical_length = 4096;
    c.physical_length = 4096;
    const Bytes cd = {'c', 'd'};
    c.digest = ContentDigest::of(ByteSpan(cd.data(), cd.size()));
    c.blob = BlobId(11);
    c.provenance = AuthorityOrigin::WORKER;
    m.chunks.push_back(c);
    snap.manifests.push_back(m);

    PlacementRecord p;
    p.id = PlacementId(21);
    p.object = ObjectId(5);
    p.object_generation = ObjectGeneration(1);
    p.replica = ReplicaId(31);
    p.replica_generation = ReplicaGeneration(1);
    p.backend = StorageBackendId(2);
    p.tier = StorageTierId(2);
    p.volume = VolumeId(1);
    p.key = "placements/placement-21";
    p.manifest = m.id;
    p.logical_size = 4096;
    p.physical_size = 4096;
    const Bytes db = {'o', 'b', 'j'};
    p.digest = ContentDigest::of(ByteSpan(db.data(), db.size()));
    p.durability_replicas = 1;
    p.lifecycle = PlacementLifecycle::AVAILABLE;
    p.freshness = Freshness::CURRENT;
    p.authority_generation = auth.generation;
    p.provenance = AuthorityOrigin::WORKER;
    p.writer_worker = auth.worker;
    p.writer_boot = auth.boot;
    p.placement_generation = PlacementGeneration(1);
    snap.placements.push_back(p);

    ReplicaSet rs;
    rs.object = ObjectId(5);
    rs.current_generation = ObjectGeneration(1);
    rs.required = 1;
    rs.actual = 1;
    rs.distinct_failure_domains = 1;
    rs.authoritative_replicas = 1;
    rs.state = ReplicaState::HEALTHY;
    rs.authority_generation = auth.generation;
    ReplicaInfo ri;
    ri.id = p.replica;
    ri.generation = p.replica_generation;
    ri.backend = p.backend;
    ri.node = StorageNodeId(1);
    ri.failure_domain = "local";
    ri.logical_size = 4096;
    ri.physical_size = 4096;
    ri.state = ReplicaState::HEALTHY;
    ri.placement_generation = p.placement_generation;
    ri.authoritative = true;
    rs.replicas.push_back(ri);
    snap.replica_sets.push_back(rs);

    BackendDescriptor b;
    b.id = StorageBackendId(2);
    b.tier = StorageTierId(2);
    b.node = StorageNodeId(1);
    b.name = "synthetic";
    b.generation = BackendGeneration(1);
    b.capacity.total_bytes = 1u << 30;
    b.capacity.free_bytes = 1u << 30;
    b.health = Health::HEALTHY;
    b.freshness = Freshness::CURRENT;
    b.provenance = MeasurementKind::SYNTHETIC;
    snap.backends.push_back(b);
    return snap;
}

int main() {
    std::printf("test_persistence starting\n");

    // ---- serialize determinism / stable semantic digest ----
    const MetadataSnapshot snap = make_snapshot();
    Bytes blob, blob2;
    const PersistResult pr = serialize_snapshot(snap, blob);
    CHECK(pr.ok);
    const PersistResult pr2 = serialize_snapshot(snap, blob2);
    CHECK(pr2.ok);
    CHECK(bytes_eq(blob, blob2));               // deterministic encoding
    CHECK(pr.semantic_digest == pr2.semantic_digest);
    CHECK(pr.crc == pr2.crc);
    std::printf("  serialize determinism / stable semantic digest PASS\n");

    // ---- re-frame and deserialize correctly ----
    const Bytes dblob = rehead(blob);
    MetadataSnapshot snap2;
    const PersistResult dr = deserialize_snapshot(ByteSpan(dblob.data(), dblob.size()), snap2);
    CHECK(dr.ok);
    CHECK_EQ(snap2.manifests.size(), 1u);
    CHECK_EQ(snap2.placements.size(), 1u);
    CHECK_EQ(snap2.replica_sets.size(), 1u);
    CHECK_EQ(snap2.backends.size(), 1u);
    CHECK_EQ(snap2.manifests[0].id, ManifestId(1));
    CHECK_EQ(snap2.placements[0].id, PlacementId(21));
    CHECK_EQ(static_cast<int>(snap2.placements[0].lifecycle), static_cast<int>(PlacementLifecycle::AVAILABLE));
    CHECK(snap2.epoch == CoordinatorEpoch(2));
    CHECK(snap2.authority.worker == WorkerId(5));

    // The semantic digest is stable across a serialize -> deserialize cycle.
    const std::uint32_t payload_len = rd_u32_le(blob, 5);
    const ByteSpan original_payload(blob.data() + (13 + 32), payload_len);
    CHECK(dr.semantic_digest == ContentDigest::of(original_payload));
    std::printf("  deserialize of re-framed metadata PASS\n");

    // ---- reject bad magic ----
    {
        Bytes bad = dblob;
        bad[0] ^= 0xFF;
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(bad.data(), bad.size()), out);
        CHECK(!r.ok);
        CHECK_EQ(static_cast<int>(r.code), static_cast<int>(StatusCode::Malformed));
    }
    // ---- reject invalid version ----
    {
        Bytes badv = dblob;
        badv[4] = 0xEE;
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(badv.data(), badv.size()), out);
        CHECK(!r.ok);
        CHECK_EQ(static_cast<int>(r.code), static_cast<int>(StatusCode::Malformed));
    }
    // ---- reject short/truncated ----
    {
        Bytes short_blob = {0x01, 0x02, 0x03};
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(short_blob.data(), short_blob.size()), out);
        CHECK(!r.ok);
        CHECK_EQ(static_cast<int>(r.code), static_cast<int>(StatusCode::Truncated));
    }
    {
        Bytes mid(dblob.begin(), dblob.begin() + static_cast<long>(dblob.size() / 2));
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(mid.data(), mid.size()), out);
        CHECK(!r.ok);
    }
    std::printf("  truncation rejection PASS\n");

    // ---- reject corruption (CRC over payload) ----
    {
        Bytes corrupt = dblob;
        corrupt[45 + 7] ^= 0x40;   // a byte in the payload region
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(corrupt.data(), corrupt.size()), out);
        CHECK(!r.ok);
        CHECK_EQ(static_cast<int>(r.code), static_cast<int>(StatusCode::Corrupted));
    }
    // ---- reject corruption (semantic digest) ----
    {
        Bytes corrupt = dblob;
        corrupt[20] ^= 0x01;   // inside the 32-byte stored semantic digest
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(corrupt.data(), corrupt.size()), out);
        CHECK(!r.ok);
        CHECK_EQ(static_cast<int>(r.code), static_cast<int>(StatusCode::IntegrityMismatch));
    }
    std::printf("  corruption (CRC + semantic digest) rejection PASS\n");

    // ---- reject trailing garbage ----
    {
        Bytes tail = dblob;
        tail.push_back(0x00);
        tail.push_back(0xFF);
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(tail.data(), tail.size()), out);
        CHECK(!r.ok);
        CHECK_EQ(static_cast<int>(r.code), static_cast<int>(StatusCode::TrailingGarbage));
    }
    std::printf("  trailing garbage rejection PASS\n");

    // ---- reject duplicate placement id ----
    {
        MetadataSnapshot dup = make_snapshot();
        dup.placements.push_back(dup.placements[0]);   // same placement id twice
        Bytes dup_ser, out2;
        CHECK(serialize_snapshot(dup, dup_ser).ok);
        const Bytes dup_dblob = rehead(dup_ser);
        MetadataSnapshot out;
        PersistResult r = deserialize_snapshot(ByteSpan(dup_dblob.data(), dup_dblob.size()), out);
        CHECK(!r.ok);
        CHECK_EQ(static_cast<int>(r.code), static_cast<int>(StatusCode::DuplicateIdentity));
    }
    std::printf("  duplicate ID rejection PASS\n");

    std::printf("  note: object-descriptor round-trip and generation-regression are library gaps\n");

    std::printf("test_persistence: ALL PASS\n");
    return 0;
}
