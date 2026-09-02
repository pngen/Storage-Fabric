#include "test_util.h"
#include "storagefabric/core/planner.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/core/accounting.h"
#include "storagefabric/core/persist.h"
#include "storagefabric/model/manifest.h"
#include "storagefabric/model/placement.h"
#include "storagefabric/model/tier.h"
#include "storagefabric/model/authority.h"
#include "storagefabric/storage/local_backend.h"

#include <filesystem>
#include <vector>
#include <string>
#include <cstdint>

using namespace storagefabric;

// Deterministic PRNG so the suite is reproducible.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next64() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    std::uint64_t next(std::uint64_t n) { return n == 0 ? 0 : next64() % n; }
    std::uint32_t next_u32(std::uint32_t n) { return static_cast<std::uint32_t>(next(n)); }
    bool coin() { return (next64() & 1) != 0; }
};

static TierCandidate rand_candidate(Rng& rng, StorageBackendId id) {
    TierCandidate c;
    c.backend = id;
    c.tier = StorageTierId(id.value());
    c.storage_class = static_cast<StorageClass>(rng.next_u32(8));
    c.total_bytes = 1000 + rng.next(100000);
    c.free_bytes = c.total_bytes - rng.next(c.total_bytes);   // free in [1, total]
    if (c.free_bytes == 0) c.free_bytes = 1;
    c.capacity_unknown = false;
    c.read_latency_s = 1e-5 + static_cast<double>(rng.next(1000000)) * 1e-8;
    c.write_latency_s = c.read_latency_s;
    c.read_bps = 1e6 + static_cast<double>(rng.next(1000000000));
    c.write_bps = 1e6 + static_cast<double>(rng.next(1000000000));
    c.health = (rng.next(4) == 0) ? Health::DEGRADED : Health::HEALTHY;
    c.eviction_capable = true;
    c.persistent = true;
    c.failure_domain = "fd-" + std::to_string(rng.next(3));   // up to 3 domains
    c.cost_class = (rng.next(2) == 0) ? "local" : "synthetic-remote";
    c.locality = "loc";
    c.current_pressure = rng.next(c.total_bytes);
    c.provenance = (rng.next(2) == 0) ? MeasurementKind::MEASURED : MeasurementKind::SYNTHETIC;
    return c;
}

int main() {
    const std::uint64_t seed = 0xC0FFEE123456789ULL;
    std::printf("test_property starting\n  fixed seed = 0x%016llx\n", seed);
    Rng rng(seed);

    const std::size_t ITERS = 200;
    const std::size_t N = 64;
    const std::vector<std::uint8_t> content(N, 0x5A);
    ByteSpan span = ByteSpan(content.data(), content.size());

    // ---- refcount / accounting consistency across a dedup + eviction pair ----
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-test-prop";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        StorageFabric sf;
        AuthorityEnvelope auth;
        auth.epoch = CoordinatorEpoch(1);
        auth.boot = WorkerBootId(100);
        auth.worker = WorkerId(7);
        auth.generation = AuthorityGeneration(1);
        auth.origin = AuthorityOrigin::RUNNER;
        sf.set_authority(auth);
        auto bid = sf.register_local_backend("local", root, StorageClass::LOCAL_FILESYSTEM);
        CHECK_OK(bid);
        DurabilityRequirement dur;
        dur.min_replicas = 1;
        auto o1 = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
        CHECK_OK(o1);
        auto o2 = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
        CHECK_OK(o2);
        PublishOptions opts;
        opts.required_replicas = 1;
        opts.dedupe = true;
        auto p1 = sf.publish_to(o1.value(), span, bid.value(), opts);
        CHECK_OK(p1);
        auto p2 = sf.publish_to(o2.value(), span, bid.value(), opts);
        CHECK_OK(p2);
        CHECK_EQ(sf.accounting().physical_blobs, 1u);       // dedup keeps one blob
        CHECK(sf.accounting().is_consistent());             // refcount never negative
        // Evicting the last authoritative copy is rejected; accounting stays consistent.
        const EvictionDecision d = sf.can_evict(p1.value().id);
        CHECK(!d.allowed);
        CHECK(sf.accounting().is_consistent());
        std::filesystem::remove_all(root, ec);
    }
    std::printf("  refcount/accounting never negative PASS\n");

    std::uint64_t check_count = 0;
    for (std::size_t iter = 0; iter < ITERS; ++iter) {
        // ---- deterministic planning ----
        std::vector<TierCandidate> cands;
        const std::uint32_t n = 1 + rng.next_u32(3);
        for (std::uint32_t i = 0; i < n; ++i)
            cands.push_back(rand_candidate(rng, StorageBackendId(static_cast<std::uint64_t>(i + 1))));
        PlanRequest req;
        req.object = ObjectId(1);
        req.object_generation = ObjectGeneration(1);
        req.kind = ObjectKind::CHECKPOINT;
        req.logical_size = 64;
        req.required_replicas = 1;
        req.candidates = cands;
        const StoragePlan sp1 = plan(req);
        const StoragePlan sp2 = plan(req);
        CHECK(sp1.feasible == sp2.feasible);
        CHECK(sp1.ranked.size() == sp2.ranked.size());
        for (std::size_t i = 0; i < sp1.ranked.size(); ++i) {
            CHECK(sp1.ranked[i].backend == sp2.ranked[i].backend);
            CHECK(sp1.ranked[i].score == sp2.ranked[i].score);
        }
        if (sp1.selected.has_value() && sp2.selected.has_value()) {
            CHECK(sp1.selected.value().backend == sp2.selected.value().backend);
        }
        ++check_count;

        // ---- canonical digest stable + chunk layout has no gap/overlap ----
        Bytes c1(N), c2(N);
        for (std::size_t i = 0; i < N; ++i) { c1[i] = static_cast<std::uint8_t>((i * 7 + iter) & 0xFF); c2[i] = c1[i]; }
        CHECK(ContentDigest::of(ByteSpan(c1.data(), c1.size())) == ContentDigest::of(ByteSpan(c2.data(), c2.size())));

        Manifest m;
        m.id = ManifestId(iter + 1);
        m.generation = ManifestGeneration(1);
        m.object = ObjectId(1);
        m.object_generation = ObjectGeneration(1);
        m.total_logical_length = 128;
        const Bytes mdb = {'m'};
        m.manifest_digest = ContentDigest::of(ByteSpan(mdb.data(), mdb.size()));
        // Contiguous, ordered chunks -> no gap/overlap.
        std::uint64_t off = 0;
        std::uint64_t cid = 1;
        while (off < 128) {
            const std::uint64_t len = std::min<std::uint64_t>(1 + rng.next(40), 128 - off);
            ChunkDescriptor c;
            c.id = ChunkId(cid);
            c.generation = ChunkGeneration(1);
            c.offset = off;
            c.logical_length = len;
            c.physical_length = len;
            const Bytes cdb = {static_cast<std::uint8_t>(cid)};
            c.digest = ContentDigest::of(ByteSpan(cdb.data(), cdb.size()));
            c.blob = BlobId(cid);
            c.provenance = AuthorityOrigin::WORKER;
            m.chunks.push_back(c);
            off += len;
            ++cid;
        }
        m.sort_chunks();
        CHECK_STATUS(m.validate());                          // no gap/overlap/non-cover
        const auto before = m.chunks;
        m.sort_chunks();
        CHECK(m.chunks.size() == before.size());

        // ---- stale authority never fences a fresh boot ----
        AuthorityEnvelope fresh;
        fresh.epoch = CoordinatorEpoch(10);
        fresh.boot = WorkerBootId(1000);
        fresh.worker = WorkerId(1);
        fresh.generation = AuthorityGeneration(1);
        fresh.origin = AuthorityOrigin::WORKER;
        AuthorityEnvelope staleBoot;
        staleBoot.epoch = CoordinatorEpoch(1);
        staleBoot.boot = WorkerBootId(50);
        staleBoot.worker = WorkerId(1);
        staleBoot.generation = AuthorityGeneration(999999);
        staleBoot.origin = AuthorityOrigin::WORKER;
        CHECK(!staleBoot.is_strictly_newer_than(fresh));
        CHECK(fresh.is_strictly_newer_than(staleBoot));

        // ---- recovered/incomplete write is never AVAILABLE ----
        PlacementRecord pr;
        pr.lifecycle = PlacementLifecycle::WRITING;
        CHECK(!pr.is_authoritative());
        CHECK(!can_transition(PlacementLifecycle::WRITING, PlacementLifecycle::AVAILABLE));
        CHECK(can_transition(PlacementLifecycle::VERIFYING, PlacementLifecycle::AVAILABLE));
        CHECK(!can_transition(PlacementLifecycle::PLANNED, PlacementLifecycle::AVAILABLE));
        PlacementRecord avail;
        avail.lifecycle = PlacementLifecycle::AVAILABLE;
        CHECK(avail.is_authoritative());

        // ---- UNKNOWN never becomes MEASURED ----
        TypedObservation<std::uint64_t> obs;
        obs.value = 42;
        obs.origin = MeasurementKind::UNKNOWN;
        CHECK(!obs.is_known());
        CHECK_EQ(static_cast<int>(obs.origin), static_cast<int>(MeasurementKind::UNKNOWN));
        CHECK(parse_measurement_kind(to_string(MeasurementKind::UNKNOWN)).value() == MeasurementKind::UNKNOWN);
        const auto parsed = parse_measurement_kind("MEASURED");
        CHECK(parsed.has_value());
        CHECK(static_cast<int>(parsed.value()) != static_cast<int>(MeasurementKind::UNKNOWN));
        // A candidate reported as UNKNOWN remains non-MEASURED in the plan factors.
        TierCandidate u;
        u.provenance = MeasurementKind::UNKNOWN;
        PlanRequest ur;
        ur.object = ObjectId(1);
        ur.object_generation = ObjectGeneration(1);
        ur.kind = ObjectKind::CHECKPOINT;
        ur.logical_size = 4;
        ur.required_replicas = 1;
        ur.candidates = {u};
        StoragePlan usp = plan(ur);
        if (usp.selected.has_value()) {
            for (const auto& f : usp.selected.value().factors) {
                if (f.name != "cost") {
                    CHECK_EQ(static_cast<int>(f.origin), static_cast<int>(PlanFactorOrigin::UNKNOWN));
                }
            }
        }
        ++check_count;
    }

    std::printf("  %zu iterations: deterministic planning, stable digest, no gap/overlap,\n", ITERS);
    std::printf("    no stale commit, recovered-write never AVAILABLE, UNKNOWN stays UNKNOWN PASS\n");

    // ---- persistence semantic digest is stable across serialization ----
    {
        MetadataSnapshot snap;
        snap.epoch = CoordinatorEpoch(3);
        AuthorityEnvelope a;
        a.epoch = CoordinatorEpoch(3);
        a.boot = WorkerBootId(11);
        a.worker = WorkerId(2);
        a.generation = AuthorityGeneration(4);
        a.origin = AuthorityOrigin::RUNNER;
        snap.authority = a;
        snap.policy_generation = PolicyGeneration(2);
        snap.saved_at_ms = 321;
        snap.accounting.logical_bytes = 7;
        snap.accounting.manifests = 1;
        Manifest m;
        m.id = ManifestId(9);
        m.generation = ManifestGeneration(1);
        m.object = ObjectId(3);
        m.object_generation = ObjectGeneration(1);
        m.total_logical_length = 4;
        const Bytes mdb = {'m'};
        m.manifest_digest = ContentDigest::of(ByteSpan(mdb.data(), mdb.size()));
        ChunkDescriptor c;
        c.id = ChunkId(1);
        c.generation = ChunkGeneration(1);
        c.offset = 0;
        c.logical_length = 4;
        c.physical_length = 4;
        const Bytes cdb = {'c'};
        c.digest = ContentDigest::of(ByteSpan(cdb.data(), cdb.size()));
        c.blob = BlobId(1);
        c.provenance = AuthorityOrigin::WORKER;
        m.chunks.push_back(c);
        snap.manifests.push_back(m);

        Bytes b1, b2;
        const PersistResult r1 = serialize_snapshot(snap, b1);
        const PersistResult r2 = serialize_snapshot(snap, b2);
        CHECK(r1.ok);
        CHECK(r2.ok);
        CHECK(r1.semantic_digest == r2.semantic_digest);   // canonical digest preserved
        CHECK(bytes_eq(b1, b2));
    }
    std::printf("  persistence semantic digest stable PASS\n");

    std::printf("test_property: ALL PASS (%zu checks)\n", check_count);
    return 0;
}
