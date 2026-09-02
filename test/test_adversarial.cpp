#include "test_util.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/core/capacity.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/protocol.h"
#include "storagefabric/core/persist.h"
#include "storagefabric/model/manifest.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/storage/synthetic_backend.h"

#include <filesystem>
#include <vector>
#include <string>

using namespace storagefabric;

int main() {
    std::printf("test_adversarial starting\n");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-test-adv";
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

    // ---- zero-length object rejected ----
    {
        const std::vector<std::uint8_t> empty;
        auto obj = sf.define_object(ObjectKind::CHECKPOINT, 0, ByteSpan(empty.data(), empty.size()),
                                    auth, dur);
        CHECK(obj.failed());
        CHECK_EQ(static_cast<int>(obj.error_code()), static_cast<int>(StatusCode::InvalidArgument));
    }
    // ---- content length mismatch ----
    {
        const std::vector<std::uint8_t> c(8, 1);
        auto obj = sf.define_object(ObjectKind::CHECKPOINT, 16, ByteSpan(c.data(), c.size()),
                                    auth, dur);
        CHECK(obj.failed());
        CHECK_EQ(static_cast<int>(obj.error_code()), static_cast<int>(StatusCode::LengthMismatch));
    }
    std::printf("  zero-length + length-mismatch objects rejected PASS\n");

    // ---- planner: object too large ----
    {
        // oversized object vs a max_object_bytes limit in the planner
        const auto content = std::vector<std::uint8_t>(1000, 7);
        ByteSpan span = ByteSpan(content.data(), content.size());
        auto obj = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
        CHECK_OK(obj);
        auto req = sf.make_plan_request(obj.value(), 1);
        req.max_object_bytes = 128;
        const StoragePlan sp = sf.plan(req);
        CHECK(!sp.feasible);
    }
    // ---- publish exceeds backend capacity -> insufficient ----
    {
        const auto content = std::vector<std::uint8_t>(200 * 1024, 9);
        ByteSpan span = ByteSpan(content.data(), content.size());
        auto obj = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
        CHECK_OK(obj);
        StorageFabric small;
        small.set_authority(auth);
        auto tiny = small.register_synthetic_backend("tiny", [] {
            SyntheticProfile p;
            p.total_bytes = 1024;
            p.free_bytes = 1024;
            p.health = Health::HEALTHY;
            return p;
        }());
        CHECK_OK(tiny);
        PublishOptions opts;
        opts.required_replicas = 1;
        auto place = small.publish_to(obj.value(), span, tiny.value(), opts);
        CHECK(place.failed());
        CHECK_EQ(static_cast<int>(place.error_code()), static_cast<int>(StatusCode::InsufficientCapacity));
    }
    std::printf("  oversized object + capacity rejection PASS\n");

    // ---- manifest offset overflow ----
    {
        Manifest m;
        m.id = ManifestId(1);
        m.object = ObjectId(1);
        m.total_logical_length = 100;
        const Bytes md = {'m'};
        m.manifest_digest = ContentDigest::of(ByteSpan(md.data(), md.size()));
        ChunkDescriptor c;
        c.id = ChunkId(1);
        c.generation = ChunkGeneration(1);
        c.offset = 150;   // beyond the object length
        c.logical_length = 10;
        c.physical_length = 10;
        const Bytes cd = {'c'};
        c.digest = ContentDigest::of(ByteSpan(cd.data(), cd.size()));
        c.blob = BlobId(1);
        m.chunks.push_back(c);
        const auto mv = validate_manifest(m, false);
        CHECK(!mv.ok);
        CHECK_EQ(static_cast<int>(mv.code), static_cast<int>(StatusCode::Overflow));
    }
    // ---- duplicate chunk id ----
    {
        Manifest m;
        m.id = ManifestId(1);
        m.object = ObjectId(1);
        m.total_logical_length = 100;
        const Bytes md = {'m'};
        m.manifest_digest = ContentDigest::of(ByteSpan(md.data(), md.size()));
        ChunkDescriptor a = {ChunkId(1), ChunkGeneration(1), 0, 50, 50, ContentDigest::of(ByteSpan(reinterpret_cast<const std::uint8_t*>("a"), 1)), BlobId(1), AuthorityOrigin::WORKER};
        ChunkDescriptor b = {ChunkId(1), ChunkGeneration(1), 50, 50, 50, ContentDigest::of(ByteSpan(reinterpret_cast<const std::uint8_t*>("b"), 1)), BlobId(2), AuthorityOrigin::WORKER};
        m.chunks = {a, b};
        const auto mv = validate_manifest(m, false);
        CHECK(!mv.ok);
        CHECK_EQ(static_cast<int>(mv.code), static_cast<int>(StatusCode::DuplicateIdentity));
    }
    std::printf("  manifest offset-overflow + duplicate chunk id PASS\n");

    // ---- path traversal ----
    {
        const auto content = std::vector<std::uint8_t>(16, 3);
        ByteSpan span = ByteSpan(content.data(), content.size());
        auto p = sf.backend(bid.value())->put(span, "../escape");
        CHECK(p.failed());
        CHECK_EQ(static_cast<int>(p.error_code()), static_cast<int>(StatusCode::PathUnsafe));
        auto p2 = sf.backend(bid.value())->put(span, "/abs");
        CHECK(p2.failed());
        CHECK_EQ(static_cast<int>(p2.error_code()), static_cast<int>(StatusCode::PathUnsafe));
    }
    std::printf("  path traversal rejected PASS\n");

    // ---- reservation overcommit + duplicate commit + double release + stale gen ----
    {
        ReservationLedger led;
        BackendCapacity cap;
        cap.total_bytes = 1000;
        cap.free_bytes = 1000;
        cap.unknown = false;
        led.register_backend(StorageBackendId(1), cap);

        // overcommit
        auto over = led.reserve(StorageBackendId(1), 2000, ReservationGeneration(1),
                                WorkerId(1), "over");
        CHECK(over.failed());
        CHECK_EQ(static_cast<int>(over.error_code()), static_cast<int>(StatusCode::InsufficientCapacity));

        // duplicate commit
        auto r = led.reserve(StorageBackendId(1), 100, ReservationGeneration(1), WorkerId(1), "a");
        CHECK_OK(r);
        CHECK_STATUS(led.commit(r.value().id, PlacementId(5)));
        const Status commit2 = led.commit(r.value().id, PlacementId(6));
        CHECK(commit2.failed());
        CHECK_EQ(static_cast<int>(commit2.code()), static_cast<int>(StatusCode::InvalidState));

        // double release via guard
        auto r2 = led.reserve(StorageBackendId(1), 100, ReservationGeneration(1), WorkerId(1), "b");
        CHECK_OK(r2);
        ReservationGuard g(&led, r2.value());
        CHECK_STATUS(g.release());
        const Status rel2 = g.release();
        CHECK(rel2.failed());
        CHECK_EQ(static_cast<int>(rel2.code()), static_cast<int>(StatusCode::DuplicateReservation));

        // stale generation release
        auto r3 = led.reserve(StorageBackendId(1), 100, ReservationGeneration(1), WorkerId(1), "c");
        CHECK_OK(r3);
        const Status stag = led.release(r3.value().id, ReservationGeneration(2));  // wrong gen
        CHECK(stag.failed());
        CHECK_EQ(static_cast<int>(stag.code()), static_cast<int>(StatusCode::StaleReservation));
        std::printf("  reservation overcommit/dup-commit/double-release/stale-gen PASS\n");
    }

    // ---- stale authority ----
    {
        AuthorityEnvelope cur;
        cur.epoch = CoordinatorEpoch(2);
        cur.boot = WorkerBootId(300);
        cur.worker = WorkerId(1);
        cur.generation = AuthorityGeneration(5);
        cur.origin = AuthorityOrigin::WORKER;
        AuthorityEnvelope stale;
        stale.epoch = CoordinatorEpoch(1);
        stale.boot = WorkerBootId(100);
        stale.worker = WorkerId(1);
        stale.generation = AuthorityGeneration(999);
        stale.origin = AuthorityOrigin::WORKER;
        CHECK(!stale.is_strictly_newer_than(cur));
        CHECK(!is_authoritative_after(stale, cur));
        std::printf("  stale authority rejected PASS\n");
    }

    // ---- degraded backend: completion after failure -> no placement ----
    // ---- unavailable backend: publish/read rejected ----
    {
        StorageFabric dsf;
        dsf.set_authority(auth);
        SyntheticProfile p;
        p.health = Health::HEALTHY;
        p.total_bytes = 1u << 30;
        p.free_bytes = 1u << 30;
        auto sbid = dsf.register_synthetic_backend("syn", p);
        CHECK_OK(sbid);
        auto* syn = dynamic_cast<SyntheticBackend*>(dsf.backend(sbid.value()));
        CHECK(syn != nullptr);

        const auto content = std::vector<std::uint8_t>(256, 5);
        ByteSpan span = ByteSpan(content.data(), content.size());
        auto obj = dsf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
        CHECK_OK(obj);
        const std::size_t before = dsf.placements().size();

        // degraded: writes fail, reads allowed
        syn->set_health(Health::DEGRADED, true, false);
        PublishOptions opts;
        opts.required_replicas = 1;
        auto placeDeg = dsf.publish_to(obj.value(), span, sbid.value(), opts);
        CHECK(placeDeg.failed());
        CHECK_EQ(static_cast<int>(placeDeg.error_code()), static_cast<int>(StatusCode::BackendDegraded));
        CHECK_EQ(dsf.placements().size(), before);   // no placement recorded after failure

        // unavailable: both read and write rejected
        syn->set_health(Health::UNAVAILABLE, false, true);
        auto placeUn = dsf.publish_to(obj.value(), span, sbid.value(), opts);
        CHECK(placeUn.failed());
        CHECK_EQ(static_cast<int>(placeUn.error_code()), static_cast<int>(StatusCode::BackendUnavailable));
        std::printf("  degraded/unavailable backend rejection + no placement after failure PASS\n");
    }

    // ---- malformed persistence + malformed protocol frame ----
    {
        MetadataSnapshot out;
        Bytes shortblob = {1, 2, 3};
        PersistResult pr = deserialize_snapshot(ByteSpan(shortblob.data(), shortblob.size()), out);
        CHECK(!pr.ok);
        CHECK_EQ(static_cast<int>(pr.code), static_cast<int>(StatusCode::Truncated));

        Bytes badframe = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        std::size_t consumed = 0;
        auto df = decode_frame(ByteSpan(badframe.data(), badframe.size()), consumed);
        CHECK(!df.ok());
        CHECK_EQ(static_cast<int>(df.error_code()), static_cast<int>(StatusCode::Truncated));
    }
    std::printf("  malformed persistence + malformed protocol frame PASS\n");

    std::filesystem::remove_all(root, ec);
    std::printf("test_adversarial: ALL PASS\n");
    return 0;
}
