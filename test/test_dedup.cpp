#include "test_util.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/core/accounting.h"

#include <filesystem>
#include <vector>

using namespace storagefabric;

int main() {
    std::printf("test_dedup starting\n");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-test-dedup";
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

    // Content X (identical for both logical objects).
    const std::size_t N = 4096;
    std::vector<std::uint8_t> content(N);
    for (std::size_t i = 0; i < N; ++i) content[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    ByteSpan span = ByteSpan(content.data(), content.size());

    DurabilityRequirement dur;
    dur.min_replicas = 1;

    // Two identical logical objects.
    auto objA = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
    CHECK_OK(objA);
    auto objB = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
    CHECK_OK(objB);

    PublishOptions opts;
    opts.required_replicas = 1;
    opts.dedupe = true;

    auto pa = sf.publish_to(objA.value(), span, bid.value(), opts);
    CHECK_OK(pa);
    auto pb = sf.publish_to(objB.value(), span, bid.value(), opts);
    CHECK_OK(pb);

    // Both logical objects share one physical blob (dedup).
    const AccountingTotals acc = sf.accounting();
    CHECK_EQ(acc.logical_objects, 2u);
    CHECK_EQ(acc.physical_blobs, 1u);        // identical content deduped to one blob
    CHECK_EQ(acc.physical_bytes, static_cast<std::uint64_t>(N));
    CHECK(acc.is_consistent());
    // deduplicated_bytes is not populated by the runtime (kept 0); consistency still holds.
    std::printf("  two identical objects -> one physical blob PASS\n");

    // Both read back identically.
    auto ra = sf.read(objA.value().id);
    CHECK_OK(ra);
    CHECK(bytes_eq(ra.value(), content));
    auto rb = sf.read(objB.value().id);
    CHECK_OK(rb);
    CHECK(bytes_eq(rb.value(), content));
    std::printf("  both logical objects read back PASS\n");

    // Evicting the last authoritative copy of A is rejected.
    const EvictionDecision d = sf.can_evict(pa.value().id);
    CHECK(!d.allowed);
    CHECK_EQ(static_cast<int>(d.code), static_cast<int>(StatusCode::EvictionUnsafe));
    const auto ev = sf.evict(pa.value().id);
    CHECK(ev.ok());
    CHECK(!ev.value().allowed);
    CHECK_EQ(static_cast<int>(ev.value().code), static_cast<int>(StatusCode::EvictionUnsafe));
    std::printf("  evicting last authoritative copy rejected PASS\n");

    // No underflow: the shared blob is still valid for B after the rejected evict.
    auto rb2 = sf.read(objB.value().id);
    CHECK_OK(rb2);
    CHECK(bytes_eq(rb2.value(), content));
    const AccountingTotals acc2 = sf.accounting();
    CHECK(acc2.is_consistent());
    CHECK_EQ(acc2.physical_blobs, 1u);
    std::printf("  shared blob survives rejected evict without underflow PASS\n");

    std::filesystem::remove_all(root, ec);
    std::printf("test_dedup: ALL PASS\n");
    return 0;
}
