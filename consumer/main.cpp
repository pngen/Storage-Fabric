#include "storagefabric/core/runtime.h"
#include "storagefabric/core/planner.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

// Downstream package proof: consumes Storage Fabric as an installed package
// (StorageFabric::storagefabric) and exercises the real public API end-to-end.
using namespace storagefabric;

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(4242);
    auth.worker = WorkerId(9);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path rootA = std::filesystem::temp_directory_path() / "sfb-consumer-a";
    const std::filesystem::path rootB = std::filesystem::temp_directory_path() / "sfb-consumer-b";
    std::error_code ec;
    std::filesystem::remove_all(rootA, ec);
    std::filesystem::remove_all(rootB, ec);
    auto bidA = sf.register_local_backend("consumer-local-a", rootA, StorageClass::LOCAL_FILESYSTEM);
    auto bidB = sf.register_local_backend("consumer-local-b", rootB, StorageClass::LOCAL_FILESYSTEM);
    if (bidA.failed() || bidB.failed()) { std::printf("register backend failed\n"); return 1; }
    const StorageBackendId backendA = bidA.value();
    const StorageBackendId backendB = bidB.value();
    std::printf("registered local backends %s and %s\n", backendA.str().c_str(), backendB.str().c_str());

    // ---- object descriptor ----
    const std::size_t size = 32 * 1024;
    std::vector<std::uint8_t> content(size);
    for (std::size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 17 + 3) & 0xFF);
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::CHECKPOINT, content.size(),
                                    ByteSpan(content.data(), content.size()), auth, dur, RestorePriority::HIGH);
    if (obj_res.failed()) { std::printf("define_object failed: %s\n", obj_res.error_message().c_str()); return 1; }
    const ObjectDescriptor obj = obj_res.value();
    std::printf("object descriptor: id=%s generation=%s kind=%s digest=%s\n",
                obj.id.str().c_str(), obj.generation.str().c_str(), to_string(obj.kind),
                obj.digest.short_hex(14).c_str());

    // ---- placement plan ----
    const PlanRequest req = sf.make_plan_request(obj, 1);
    const StoragePlan sp = sf.plan(req);
    std::printf("placement plan: feasible=%s selected_backend=%s\n",
                sp.feasible ? "yes" : "no", sp.selected ? sp.selected->backend.str().c_str() : "-");

    // ---- publish ----
    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    opts.eager_verify = true;
    auto pl = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
    if (pl.failed()) { std::printf("publish failed: %s\n", pl.error_message().c_str()); return 1; }
    const PlacementRecord p = pl.value();
    std::printf("published: placement=%s backend=%s lifecycle=%s freshness=%s\n",
                p.id.str().c_str(), p.backend.str().c_str(), to_string(p.lifecycle), to_string(p.freshness));

    // ---- verify digest ----
    auto vr = sf.verify(p.id);
    std::printf("verify: ok=%s code=%s size=%zu\n",
                vr.ok() ? "yes" : "no", status_name(vr.ok() ? vr.value().code : vr.error_code()),
                vr.ok() ? vr.value().size : 0);

    // ---- read back ----
    auto rd = sf.read(obj.id);
    const bool match = rd.ok() && rd.value().size() == content.size() &&
                       std::memcmp(rd.value().data(), content.data(), content.size()) == 0;
    std::printf("read-back: bytes=%zu match=%s\n", rd.ok() ? rd.value().size() : 0, match ? "yes" : "no");

    // ---- explain placement ----
    std::printf("explain placement: %s\n", sf.explain_placement(obj.id).c_str());

    // ---- evict safely / release ----
    const EvictionDecision rej = sf.can_evict(p.id);
    std::printf("can_evict(sole authoritative copy) allowed=%s (%s)\n",
                rej.allowed ? "yes" : "no", rej.reason.c_str());

    // Move to the second backend, then evict the (now stale) source safely.
    PublishOptions cross;
    cross.authority = auth;
    cross.required_replicas = 1;
    cross.dedupe = false;   // cross-backend copy writes its own blob
    auto mv = sf.move(p, backendB, cross);
    if (mv.failed()) { std::printf("move failed: %s\n", mv.error_message().c_str()); return 1; }
    const PlacementRecord dest = mv.value();
    std::printf("moved to backend=%s (destination=%s, source demoted to STALE)\n",
                dest.backend.str().c_str(), dest.id.str().c_str());
    const EvictionDecision stale_ok = sf.can_evict(p.id);
    std::printf("can_evict(stale source) allowed=%s (%s)\n", stale_ok.allowed ? "yes" : "no", stale_ok.reason.c_str());
    if (stale_ok.allowed) {
        auto ev = sf.evict(p.id);
        std::printf("evict(stale source) ok=%s reason=%s\n",
                    ev.ok() ? "yes" : "no", ev.ok() ? ev.value().reason.c_str() : ev.error_message().c_str());
    }

    // ---- clean accounting ----
    const AccountingTotals a = sf.accounting();
    std::printf("clean accounting: logical_objects=%llu logical_bytes=%llu physical_blobs=%llu "
                "physical_bytes=%llu active_placements=%llu committed_bytes=%llu consistent=%s\n",
                (unsigned long long)a.logical_objects, (unsigned long long)a.logical_bytes,
                (unsigned long long)a.physical_blobs, (unsigned long long)a.physical_bytes,
                (unsigned long long)a.active_placements, (unsigned long long)a.committed_bytes,
                a.is_consistent() ? "yes" : "no");
    std::printf("replica set: %s\n", sf.explain_replication(obj.id).c_str());
    auto final_read = sf.read(obj.id);
    const bool final_ok = final_read.ok() && final_read.value().size() == content.size() &&
                          std::memcmp(final_read.value().data(), content.data(), content.size()) == 0;
    std::printf("read after eviction of stale copy (authoritative remains): bytes=%zu match=%s\n",
                final_read.ok() ? final_read.value().size() : 0, final_ok ? "yes" : "no");

    std::filesystem::remove_all(rootA, ec);
    std::filesystem::remove_all(rootB, ec);
    std::printf("CONSUMER_OK\n");
    return 0;
}
