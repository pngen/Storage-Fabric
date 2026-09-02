#include "test_util.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/model/enums.h"

#include <filesystem>
#include <vector>
#include <string>

using namespace storagefabric;

int main() {
    std::printf("test_restore starting\n");

    // ---- restore priority classes: to_string / parse round-trip ----
    const std::initializer_list<RestorePriority> prios = {
        RestorePriority::CRITICAL, RestorePriority::HIGH, RestorePriority::NORMAL,
        RestorePriority::LOW, RestorePriority::BACKGROUND};
    for (const RestorePriority rp : prios) {
        const char* s = to_string(rp);
        CHECK(s != nullptr);
        const auto parsed = parse_restore_priority(s);
        CHECK(parsed.has_value());
        CHECK_EQ(static_cast<int>(parsed.value()), static_cast<int>(rp));
    }
    CHECK(!parse_restore_priority("urgent").has_value());
    CHECK_EQ(static_cast<int>(RestorePriority::CRITICAL), 0);
    CHECK_EQ(static_cast<int>(RestorePriority::BACKGROUND), 4);
    std::printf("  restore priority classes PASS\n");

    // ---- explain_restore and authoritative-source restore plan ----
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-test-restore";
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

    const std::size_t N = 768;
    std::vector<std::uint8_t> content(N);
    for (std::size_t i = 0; i < N; ++i) content[i] = static_cast<std::uint8_t>((i * 19 + 1) & 0xFF);
    ByteSpan span = ByteSpan(content.data(), content.size());

    DurabilityRequirement dur;
    dur.min_replicas = 1;

    auto obj = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur,
                                RestorePriority::HIGH);
    CHECK_OK(obj);
    CHECK_EQ(static_cast<int>(obj.value().restore_priority), static_cast<int>(RestorePriority::HIGH));

    PublishOptions opts;
    opts.required_replicas = 1;
    auto place = sf.publish_to(obj.value(), span, bid.value(), opts);
    CHECK_OK(place);

    // explain_restore names the object, priority, and authoritative source.
    const std::string ex = sf.explain_restore(obj.value().id);
    CHECK(ex.find("Restore of") != std::string::npos);
    CHECK(ex.find("priority HIGH") != std::string::npos);
    CHECK(ex.find("will read from the authoritative replica first") != std::string::npos);
    std::printf("  explain_restore names priority and authoritative source PASS\n");

    // The restore source is the authoritative AVAILABLE placement.
    const auto authp = sf.find_authoritative_placement(obj.value().id);
    CHECK_OK(authp);
    CHECK_EQ(static_cast<int>(authp.value().lifecycle), static_cast<int>(PlacementLifecycle::AVAILABLE));
    CHECK(authp.value().is_authoritative());

    // Simulating a restore: the object is re-materialized from that source.
    auto rd = sf.read(obj.value().id, authp.value().id);
    CHECK_OK(rd);
    CHECK(bytes_eq(rd.value(), content));
    std::printf("  restore plan reads from authoritative source PASS\n");

    // A lower-priority object still resolves to its authoritative source.
    auto obj2 = sf.define_object(ObjectKind::KV_STATE, content.size(), span, auth, dur,
                                 RestorePriority::BACKGROUND);
    CHECK_OK(obj2);
    auto place2 = sf.publish_to(obj2.value(), span, bid.value(), opts);
    CHECK_OK(place2);
    CHECK(sf.explain_restore(obj2.value().id).find("priority BACKGROUND") != std::string::npos);

    std::filesystem::remove_all(root, ec);
    std::printf("test_restore: ALL PASS\n");
    return 0;
}
