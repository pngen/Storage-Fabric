#include "test_util.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"

#include <filesystem>
#include <vector>
#include <string>

using namespace storagefabric;

int main() {
    std::printf("test_eviction starting\n");

    const std::filesystem::path root1 = std::filesystem::temp_directory_path() / "sfb-test-evic-1";
    const std::filesystem::path root2 = std::filesystem::temp_directory_path() / "sfb-test-evic-2";
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

    const std::size_t N = 512;
    std::vector<std::uint8_t> content(N);
    for (std::size_t i = 0; i < N; ++i) content[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    ByteSpan span = ByteSpan(content.data(), content.size());

    DurabilityRequirement dur;
    dur.min_replicas = 1;

    // --- object A: cannot evict the last authoritative copy ---
    auto objA = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
    CHECK_OK(objA);
    PublishOptions opts;
    opts.required_replicas = 1;
    opts.dedupe = false;
    auto pA = sf.publish_to(objA.value(), span, l1.value(), opts);
    CHECK_OK(pA);

    // Retention policy is not consulted by can_evict: the survival gate is the
    // authoritative-replica count (see the eviction driver in runtime.cpp).
    const EvictionDecision d = sf.can_evict(pA.value().id);
    CHECK(!d.allowed);
    CHECK_EQ(static_cast<int>(d.code), static_cast<int>(StatusCode::EvictionUnsafe));
    CHECK(d.reason.find("would reduce authoritative replicas below required") != std::string::npos);

    // explain_eviction reports why.
    const std::string ex = sf.explain_eviction(pA.value().id);
    CHECK(ex.find("Eviction rejected") != std::string::npos);
    CHECK(ex.find("would reduce authoritative replicas") != std::string::npos);
    std::printf("  cannot evict last authoritative copy PASS\n");

    // evict() likewise rejects.
    const auto ev = sf.evict(pA.value().id);
    CHECK(ev.ok());
    CHECK(!ev.value().allowed);
    CHECK_EQ(static_cast<int>(ev.value().code), static_cast<int>(StatusCode::EvictionUnsafe));
    std::printf("  evict() rejects last authoritative copy PASS\n");

    // --- object B: eviction succeeds after the placement is no longer authoritative ---
    auto objB = sf.define_object(ObjectKind::KV_STATE, content.size(), span, auth, dur);
    CHECK_OK(objB);
    PublishOptions bopts;
    bopts.required_replicas = 1;
    bopts.dedupe = false;
    auto q1 = sf.publish_to(objB.value(), span, l1.value(), bopts);
    CHECK_OK(q1);

    // move() creates a fresh authoritative copy on l2 and demotes the source.
    auto q2 = sf.move(q1.value(), l2.value(), bopts);
    CHECK_OK(q2);

    // q1 is now STALE, so it is not counted as the required authoritative copy.
    const auto stale = [&]() {
        for (const auto& p : sf.placements()) if (p.id == q1.value().id) return p;
        return PlacementRecord{};
    }();
    CHECK_EQ(static_cast<int>(stale.lifecycle), static_cast<int>(PlacementLifecycle::STALE));

    const EvictionDecision d2 = sf.can_evict(q1.value().id);
    CHECK(d2.allowed);
    CHECK_EQ(static_cast<int>(d2.code), static_cast<int>(StatusCode::Ok));
    auto ev2 = sf.evict(q1.value().id);
    CHECK_OK(ev2);
    CHECK(ev2.value().allowed);
    std::printf("  eviction succeeds after placement is demoted PASS\n");

    // The surviving authoritative copy keeps the object readable.
    auto rd = sf.read(objB.value().id);
    CHECK_OK(rd);
    CHECK(bytes_eq(rd.value(), content));
    const auto authp = sf.find_authoritative_placement(objB.value().id);
    CHECK_OK(authp);
    CHECK(authp.value().is_authoritative());
    std::printf("  surviving authoritative copy serves reads PASS\n");

    std::filesystem::remove_all(root1, ec);
    std::filesystem::remove_all(root2, ec);
    std::printf("test_eviction: ALL PASS\n");
    return 0;
}
