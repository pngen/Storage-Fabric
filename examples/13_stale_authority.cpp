#include "storagefabric/core/runtime.h"
#include "storagefabric/model/authority.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace storagefabric;

int main() {
    StorageFabric sf;
    AuthorityEnvelope fresh;
    fresh.epoch = CoordinatorEpoch(100);
    fresh.boot = WorkerBootId(9000);
    fresh.worker = WorkerId(5);
    fresh.generation = AuthorityGeneration(1);
    fresh.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(fresh);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex13";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("authority-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed\n"); return 1; }

    // A STALE process incarnation from an earlier epoch, but with a huge LOCAL generation.
    AuthorityEnvelope stale;
    stale.epoch = CoordinatorEpoch(50);          // older epoch
    stale.boot = WorkerBootId(9000);
    stale.worker = WorkerId(5);
    stale.generation = AuthorityGeneration(1000000);   // huge local gen
    stale.origin = AuthorityOrigin::WORKER;

    std::printf("== Authority enclosure & generation fencing ==\n");
    std::printf("  fresh authority: %s\n", fresh.describe().c_str());
    std::printf("  stale authority: %s\n", stale.describe().c_str());
    std::printf("  compare(fresh, stale)=%d (fresh is strictly newer: %s)\n",
                AuthorityEnvelope::compare(fresh, stale),
                fresh.is_strictly_newer_than(stale) ? "yes" : "no");
    std::printf("  stale.is_strictly_newer_than(fresh)=%s (fenced despite huge local generation)\n",
                stale.is_strictly_newer_than(fresh) ? "yes" : "no");
    std::printf("  is_authoritative_after(stale, fresh)=%s (mutation fenced)\n",
                is_authoritative_after(stale, fresh) ? "yes" : "no");
    std::printf("  stale.generation_fresh_after(AuthorityGeneration(999999))=%s (local gen is large but epoch stale)\n",
                stale.generation_fresh_after(AuthorityGeneration(999999)) ? "yes" : "no");

    // Publish with the FRESH authority; the placement records the accepted authority generation.
    std::vector<std::uint8_t> content(20 * 1024);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 43 + 17) & 0xFF);
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::KV_STATE, content.size(),
                                    ByteSpan(content.data(), content.size()), fresh, dur);
    if (obj_res.failed()) { std::printf("define failed\n"); return 1; }
    PublishOptions opts;
    opts.authority = fresh;
    opts.required_replicas = 1;
    auto pl = sf.publish(obj_res.value(), ByteSpan(content.data(), content.size()), opts);
    if (pl.failed()) { std::printf("publish failed: %s\n", pl.error_message().c_str()); return 1; }
    std::printf("\n  publish accepted: placement %s authority_generation=%s provenance=%s\n",
                pl.value().id.str().c_str(), pl.value().authority_generation.str().c_str(),
                to_string(pl.value().provenance));

    // A new coordinator epoch always fences the previous incarnation.
    const AuthorityEnvelope before = sf.authority();
    sf.new_coordinator_epoch();
    const AuthorityEnvelope after = sf.authority();
    std::printf("\n== Coordinator epoch discipline ==\n");
    std::printf("  before new_coordinator_epoch(): %s\n", before.describe().c_str());
    std::printf("  after  new_coordinator_epoch(): %s\n", after.describe().c_str());
    std::printf("  after is strictly newer than before: %s\n",
                after.is_strictly_newer_than(before) ? "yes" : "no");
    std::printf("  before is strictly newer than after: %s (fenced)\n",
                before.is_strictly_newer_than(after) ? "yes" : "no");
    std::printf("  a stale writer from epoch 50 can never override the fresh epoch: %s\n",
                stale.is_strictly_newer_than(after) ? "no (would be fenced)" : "correctly fenced");

    std::filesystem::remove_all(root, ec);
    std::printf("EX13_OK\n");
    return 0;
}
