#include "test_util.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/core/accounting.h"
#include "storagefabric/core/planner.h"

#include <filesystem>
#include <vector>
#include <string>

using namespace storagefabric;

int main() {
    std::printf("test_replication starting\n");

    const std::filesystem::path root1 = std::filesystem::temp_directory_path() / "sfb-test-repl-1";
    const std::filesystem::path root2 = std::filesystem::temp_directory_path() / "sfb-test-repl-2";
    std::error_code ec;
    std::filesystem::remove_all(root1, ec);
    std::filesystem::remove_all(root2, ec);

    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(100);
    auth.worker = WorkerId(7);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    auto l1 = sf.register_local_backend("local-a", root1, StorageClass::LOCAL_FILESYSTEM);
    CHECK_OK(l1);
    auto l2 = sf.register_local_backend("local-b", root2, StorageClass::LOCAL_FILESYSTEM);
    CHECK_OK(l2);

    const std::size_t N = 1024;
    std::vector<std::uint8_t> content(N);
    for (std::size_t i = 0; i < N; ++i) content[i] = static_cast<std::uint8_t>((i * 13 + 5) & 0xFF);
    ByteSpan span = ByteSpan(content.data(), content.size());

    DurabilityRequirement dur;
    dur.min_replicas = 1;

    // ---- replicate produces a second readable copy ----
    auto obj = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
    CHECK_OK(obj);
    PublishOptions opts;
    opts.required_replicas = 1;
    opts.dedupe = false;              // force the second backend to store its own copy
    auto p1 = sf.publish_to(obj.value(), span, l1.value(), opts);
    CHECK_OK(p1);

    const auto p2 = sf.replicate(p1.value(), l2.value(), opts);
    CHECK_OK(p2);

    CHECK_EQ(sf.placements().size(), 2u);       // second placement exists
    const AccountingTotals acc = sf.accounting();
    CHECK_EQ(acc.replicas, 2u);                 // replica count incremented
    CHECK_EQ(acc.active_placements, 2u);

    auto vr = sf.verify(p2.value().id);         // the new copy is verifiable
    CHECK_OK(vr);
    CHECK(vr.value().ok);
    CHECK_EQ(vr.value().size, N);
    CHECK(vr.value().digest == obj.value().digest);
    auto rd = sf.read(obj.value().id);          // the object reads back
    CHECK_OK(rd);
    CHECK(bytes_eq(rd.value(), content));
    std::printf("  replicate to second backend -> readable second copy PASS\n");

    // ---- stale replica is never authoritative ----
    auto obj2 = sf.define_object(ObjectKind::KV_STATE, content.size(), span, auth, dur);
    CHECK_OK(obj2);
    PublishOptions o2;
    o2.required_replicas = 1;
    o2.dedupe = false;
    auto q1 = sf.publish_to(obj2.value(), span, l1.value(), o2);
    CHECK_OK(q1);
    // move() replicates to l2 then demotes the source to STALE.
    auto q2 = sf.move(q1.value(), l2.value(), o2);
    CHECK_OK(q2);

    // The stale source is no longer authoritative.
    const PlacementRecord stale_source = [&]() {
        for (const auto& p : sf.placements()) if (p.id == q1.value().id) return p;
        return PlacementRecord{};
    }();
    CHECK_EQ(static_cast<int>(stale_source.lifecycle), static_cast<int>(PlacementLifecycle::STALE));
    CHECK(!stale_source.is_authoritative());
    CHECK(!stale_source.is_active());

    const auto authp = sf.find_authoritative_placement(obj2.value().id);
    CHECK_OK(authp);
    CHECK(authp.value().id == q2.value().id);   // the fresh copy is authoritative
    auto rd2 = sf.read(obj2.value().id);
    CHECK_OK(rd2);
    CHECK(bytes_eq(rd2.value(), content));
    std::printf("  stale replica never authoritative PASS\n");

    // ---- failure-domain diversity note (direct planner input) ----
    {
        PlanRequest req;
        req.object = ObjectId(99);
        req.object_generation = ObjectGeneration(1);
        req.kind = ObjectKind::CHECKPOINT;
        req.logical_size = 128;
        req.required_replicas = 2;

        TierCandidate a;
        a.backend = StorageBackendId(1);
        a.tier = StorageTierId(1);
        a.storage_class = StorageClass::LOCAL_FILESYSTEM;
        a.free_bytes = 4096;
        a.total_bytes = 8192;
        a.capacity_unknown = false;
        a.read_latency_s = 0.0001;
        a.write_latency_s = 0.0001;
        a.read_bps = 1e9;
        a.write_bps = 1e9;
        a.health = Health::HEALTHY;
        a.eviction_capable = true;
        a.persistent = true;
        a.failure_domain = "rack-a";
        a.provenance = MeasurementKind::MEASURED;

        TierCandidate b = a;
        b.backend = StorageBackendId(2);
        b.failure_domain = "rack-b";

        TierCandidate c = a;
        c.backend = StorageBackendId(3);
        c.failure_domain = "rack-a";   // same domain as a

        req.candidates = {a, b, c};
        StoragePlan sp = plan(req);
        CHECK(sp.feasible);
        CHECK_EQ(sp.replica_targets.size(), 2u);
        CHECK(sp.replica_targets[0].failure_domain == "rack-a");
        CHECK(sp.replica_targets[1].failure_domain == "rack-b");
        std::printf("  failure-domain diversity satisfied PASS\n");

        // Now only one distinct domain: under-replicated note.
        PlanRequest req2 = req;
        req2.candidates = {a, c};   // both rack-a
        StoragePlan sp2 = plan(req2);
        CHECK_EQ(sp2.replica_targets.size(), 1u);
        bool noted = false;
        for (const auto& n : sp2.notes) {
            if (n.find("failure-domain diversity") != std::string::npos) noted = true;
        }
        CHECK(noted);
        std::printf("  under-replicated diversity note PASS\n");
    }

    std::filesystem::remove_all(root1, ec);
    std::filesystem::remove_all(root2, ec);
    std::printf("test_replication: ALL PASS\n");
    return 0;
}
