#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace storagefabric;

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(88);
    auth.worker = WorkerId(17);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path rootA = std::filesystem::temp_directory_path() / "sfb-ex09-a";
    const std::filesystem::path rootB = std::filesystem::temp_directory_path() / "sfb-ex09-b";
    std::error_code ec;
    std::filesystem::remove_all(rootA, ec);
    std::filesystem::remove_all(rootB, ec);
    auto bidA = sf.register_local_backend("evict-A", rootA, StorageClass::LOCAL_FILESYSTEM);
    auto bidB = sf.register_local_backend("evict-B", rootB, StorageClass::LOCAL_FILESYSTEM);
    if (bidA.failed() || bidB.failed()) { std::printf("register failed\n"); return 1; }
    const StorageBackendId backendA = bidA.value();
    const StorageBackendId backendB = bidB.value();

    std::vector<std::uint8_t> content(24 * 1024);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 23 + 6) & 0xFF);
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::PREFIX_STATE, content.size(),
                                    ByteSpan(content.data(), content.size()), auth, dur);
    if (obj_res.failed()) { std::printf("define failed\n"); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    auto p1 = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
    if (p1.failed()) { std::printf("publish failed: %s\n", p1.error_message().c_str()); return 1; }
    const PlacementRecord sole = p1.value();

    auto placement_by_id = [&](PlacementId id) -> PlacementRecord {
        for (const auto& pl : sf.placements()) if (pl.id == id) return pl;
        return PlacementRecord{};
    };

    std::printf("== Last authoritative copy protection ==\n");
    const EvictionDecision d0 = sf.can_evict(sole.id);
    std::printf("  can_evict(sole copy %s): allowed=%s reason=%s\n",
                sole.id.str().c_str(), d0.allowed ? "yes" : "no", d0.reason.c_str());
    std::printf("  %s\n", sf.explain_eviction(sole.id).c_str());

    // Move to backend B: the source placement is demoted to STALE (not the authoritative copy).
    // Cross-backend move: disable dedup so the destination writes its own blob.
    PublishOptions cross = opts;
    cross.dedupe = false;
    auto mv = sf.move(sole, backendB, cross);
    if (mv.failed()) { std::printf("move failed: %s\n", mv.error_message().c_str()); return 1; }
    const PlacementRecord dest = mv.value();
    const PlacementRecord src_now = placement_by_id(sole.id);
    std::printf("\n== Move (source -> STALE, destination -> AVAILABLE) ==\n");
    std::printf("  source placement %s lifecycle=%s\n", src_now.id.str().c_str(), to_string(src_now.lifecycle));
    std::printf("  destination placement %s backend=%s lifecycle=%s\n",
                dest.id.str().c_str(), dest.backend.str().c_str(), to_string(dest.lifecycle));

    // The stale source is no longer authoritative, so it may be evicted.
    const EvictionDecision d1 = sf.can_evict(sole.id);
    std::printf("\n== Evict the stale source copy ==\n");
    std::printf("  can_evict(stale %s): allowed=%s reason=%s\n",
                sole.id.str().c_str(), d1.allowed ? "yes" : "no", d1.reason.c_str());
    if (d1.allowed) {
        auto ev = sf.evict(sole.id);
        std::printf("  evict(): ok=%s code=%s reason=%s\n",
                    ev.ok() ? "yes" : "no", status_name(ev.ok() ? ev.value().code : ev.error_code()),
                    ev.ok() ? ev.value().reason.c_str() : ev.error_message().c_str());
    }

    // The destination is now the only authoritative copy: evicting it must still be refused.
    const EvictionDecision d2 = sf.can_evict(dest.id);
    std::printf("\n== Protection of the remaining authoritative copy ==\n");
    std::printf("  can_evict(authoritative %s): allowed=%s reason=%s\n",
                dest.id.str().c_str(), d2.allowed ? "yes" : "no", d2.reason.c_str());
    const AccountingTotals a = sf.accounting();
    std::printf("  accounting: active_placements=%llu evicted_bytes=%llu\n",
                (unsigned long long)a.active_placements, (unsigned long long)a.evicted_bytes);
    std::printf("  %s\n", sf.explain_eviction(dest.id).c_str());

    std::filesystem::remove_all(rootA, ec);
    std::filesystem::remove_all(rootB, ec);
    std::printf("EX09_OK\n");
    return 0;
}
