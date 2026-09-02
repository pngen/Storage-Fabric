#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/storage/synthetic_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace storagefabric;

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(77);
    auth.worker = WorkerId(14);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex07";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bidLocal = sf.register_local_backend("tier-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bidLocal.failed()) { std::printf("register local failed\n"); return 1; }

    SyntheticProfile profile;
    profile.storage_class = StorageClass::SYNTHETIC_REMOTE;
    profile.total_bytes = 1ULL * 1024 * 1024 * 1024;
    profile.free_bytes = 1ULL * 1024 * 1024 * 1024;
    profile.read_latency_s = 0.001;
    profile.write_latency_s = 0.002;
    profile.read_bps = 100.0 * 1024 * 1024;
    profile.write_bps = 50.0 * 1024 * 1024;
    profile.health = Health::HEALTHY;
    profile.failure_domain = "synthetic-node";
    profile.cost_class = "synthetic-remote";
    profile.locality = "synthetic";
    profile.evictable = true;
    profile.persistent = false;
    auto bidSyn = sf.register_synthetic_backend("tier-synthetic", profile);
    if (bidSyn.failed()) { std::printf("register synthetic failed\n"); return 1; }
    const StorageBackendId local = bidLocal.value();
    const StorageBackendId synth = bidSyn.value();

    std::vector<std::uint8_t> content(64 * 1024);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 19 + 4) & 0xFF);
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::CHECKPOINT, content.size(),
                                    ByteSpan(content.data(), content.size()), auth, dur);
    if (obj_res.failed()) { std::printf("define failed\n"); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    const PlanRequest req = sf.make_plan_request(obj, 1);
    const StoragePlan sp = sf.plan(req);

    std::printf("== Tier candidates (named soft factors, as the planner sees them) ==\n");
    for (const auto& c : sp.ranked) {
        std::printf("  backend=%s tier=%s viable=%s score=%.4f\n",
                    c.backend.str().c_str(), c.tier.str().c_str(),
                    c.viable ? "yes" : "no", c.score);
        for (const auto& f : c.factors) {
            std::printf("    factor '%s' value=%.4f weight=%.4f origin=%s evidence=%s\n",
                        f.name.c_str(), f.value, f.weight, to_string(f.origin), f.evidence.c_str());
        }
    }
    if (sp.selected) {
        std::printf("  PLAN selects backend=%s tier=%s score=%.4f\n",
                    sp.selected->backend.str().c_str(), sp.selected->tier.str().c_str(), sp.selected->score);
        std::printf("  explanation: the local filesystem tier reports UNKNOWN/0 nominal read & write "
                    "measurements, so its write-bandwidth factor scores 0; the synthetic remote tier "
                    "advertises explicit SYNTHETIC measurements and therefore ranks higher.\n");
    }
    std::printf("  plan feasible=%s\n", sp.feasible ? "yes" : "no");

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    auto place = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
    if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
    std::printf("\n  object published to backend=%s\n", place.value().backend.str().c_str());
    auto rd = sf.read(obj.id);
    const bool match = rd.ok() && rd.value().size() == content.size() &&
                       std::memcmp(rd.value().data(), content.data(), content.size()) == 0;
    std::printf("  read-back: bytes=%zu match=%s\n", rd.ok() ? rd.value().size() : 0, match ? "yes" : "no");

    std::printf("\n== Tier descriptors ==\n");
    for (const auto id : {local, synth}) {
        const StorageTier* t = sf.tier(sf.backend_descriptor(id)->tier);
        const BackendDescriptor* d = sf.backend_descriptor(id);
        std::printf("  tier '%s' class=%s domain=%s durable=%s persistent=%s\n",
                    t->name.c_str(), to_string(t->storage_class), t->failure_domain.c_str(),
                    to_string(t->durability_class), t->persistent ? "yes" : "no");
        std::printf("    provenance=%s health=%s freshness=%s\n",
                    to_string(d->provenance), to_string(d->health), to_string(d->freshness));
        std::printf("    observed read_latency=%g s write_bps=%g raw_lat_s=%g\n",
                    t->read_latency_s.value, t->write_throughput_bps.value, t->write_latency_s.value);
    }
    std::printf("  synthetic tier labeled SYNTHETIC: backend=%s\n",
                sf.backend_descriptor(synth)->is_synthetic() ? "yes" : "no");

    std::filesystem::remove_all(root, ec);
    std::printf("EX07_OK\n");
    return 0;
}
