#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/storage/synthetic_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace storagefabric;

// Build a planner candidate from the LIVE backend descriptor (the registry copy is
// only updated at registration, so live set_health changes are read from the backend).
static TierCandidate live_candidate(const StorageFabric& sf, StorageBackendId id) {
    const BackendDescriptor& d = sf.backend(id)->descriptor();
    const StorageTier* t = sf.tier(d.tier);
    TierCandidate c;
    c.backend = id;
    c.tier = d.tier;
    c.storage_class = t->storage_class;
    c.free_bytes = d.capacity.free_bytes;
    c.total_bytes = d.capacity.total_bytes;
    c.capacity_unknown = d.capacity.unknown;
    c.read_latency_s = t->read_latency_s.value;
    c.write_latency_s = t->write_latency_s.value;
    c.read_bps = t->read_throughput_bps.value;
    c.write_bps = t->write_throughput_bps.value;
    c.health = d.health;
    c.eviction_capable = t->eviction_capable;
    c.persistent = t->persistent;
    c.failure_domain = t->failure_domain;
    c.cost_class = t->cost_class;
    c.locality = t->locality;
    c.current_pressure = d.capacity.committed_bytes;
    c.provenance = d.provenance;
    return c;
}

static std::vector<std::uint8_t> make_content(std::size_t n, std::uint64_t seed) {
    std::vector<std::uint8_t> v(n);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>((i * 37 + seed) & 0xFF);
    return v;
}

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(101);
    auth.worker = WorkerId(25);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex11";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bidLocal = sf.register_local_backend("health-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bidLocal.failed()) { std::printf("register local failed\n"); return 1; }
    SyntheticProfile profile;
    profile.health = Health::HEALTHY;
    profile.degraded = false;
    profile.unavailable = false;
    profile.persistent = false;
    auto bidSyn = sf.register_synthetic_backend("health-synthetic", profile);
    if (bidSyn.failed()) { std::printf("register synthetic failed\n"); return 1; }
    const StorageBackendId local = bidLocal.value();
    const StorageBackendId synth = bidSyn.value();

    std::printf("== Initial backend health (live descriptor) ==\n");
    for (const auto id : {local, synth}) {
        const BackendDescriptor& d = sf.backend(id)->descriptor();
        std::printf("  backend=%s name=%s health=%s provenance=%s synthetic=%s freshness=%s\n",
                    id.str().c_str(), d.name.c_str(), to_string(d.health), to_string(d.provenance),
                    d.is_synthetic() ? "yes" : "no", to_string(d.freshness));
    }

    // Publish a healthy object to the LOCAL backend explicitly.
    std::vector<std::uint8_t> contentA = make_content(32 * 1024, 1);
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, contentA.size(),
                                    ByteSpan(contentA.data(), contentA.size()), auth, dur);
    if (obj_res.failed()) { std::printf("define failed\n"); return 1; }
    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    auto pl = sf.publish_to(obj_res.value(), ByteSpan(contentA.data(), contentA.size()), local, opts);
    std::printf("\n  publish_to(local) -> ok=%s placement=%s\n",
                pl.ok() ? "yes" : "no", pl.ok() ? pl.value().id.str().c_str() : "-");

    auto syn_impl = static_cast<SyntheticBackend*>(sf.backend(synth));

    // DEGRADED: writes to the synthetic backend are rejected with BackendDegraded.
    syn_impl->set_health(Health::DEGRADED, true, false);
    std::vector<std::uint8_t> contentB = make_content(32 * 1024, 2);
    auto objB = sf.define_object(ObjectKind::GENERIC_AI_STATE, contentB.size(),
                                 ByteSpan(contentB.data(), contentB.size()), auth, dur);
    PublishOptions nodedup = opts;
    nodedup.dedupe = false;   // force a real write so the degraded check is hit
    auto putDeg = sf.publish_to(objB.value(), ByteSpan(contentB.data(), contentB.size()), synth, nodedup);
    std::printf("\n== Synthetic backend DEGRADED ==\n");
    std::printf("  publish_to(synthetic, dedupe=off) -> code=%s (%s)\n",
                putDeg.failed() ? status_name(putDeg.error_code()) : "Ok",
                putDeg.failed() ? putDeg.error_message().c_str() : "unexpected success");
    std::printf("  live descriptor health=%s\n", to_string(sf.backend(synth)->descriptor().health));

    // UNAVAILABLE: reads and writes are rejected; the planner sees the candidate as unavailable.
    syn_impl->set_health(Health::UNAVAILABLE, false, true);
    std::vector<std::uint8_t> contentC = make_content(32 * 1024, 3);
    auto objC = sf.define_object(ObjectKind::GENERIC_AI_STATE, contentC.size(),
                                 ByteSpan(contentC.data(), contentC.size()), auth, dur);
    auto putUnav = sf.publish_to(objC.value(), ByteSpan(contentC.data(), contentC.size()), synth, nodedup);
    std::printf("\n== Synthetic backend UNAVAILABLE ==\n");
    std::printf("  publish_to(synthetic, dedupe=off) -> code=%s (%s)\n",
                putUnav.failed() ? status_name(putUnav.error_code()) : "Ok",
                putUnav.failed() ? putUnav.error_message().c_str() : "unexpected success");
    std::printf("  live descriptor health=%s synthetic=%s\n",
                to_string(sf.backend(synth)->descriptor().health),
                sf.backend(synth)->descriptor().is_synthetic() ? "yes" : "no");

    // Planner hard constraint: the unavailable synthetic candidate is rejected.
    PlanRequest req;
    req.object = objC.value().id;
    req.object_generation = objC.value().generation;
    req.kind = objC.value().kind;
    req.logical_size = objC.value().logical_size;
    req.required_replicas = 1;
    req.policy_generation = objC.value().policy_generation;
    req.candidates.push_back(live_candidate(sf, synth));
    const StoragePlan sp = plan(req);
    std::printf("\n== Planner with only the unavailable synthetic candidate ==\n");
    std::printf("  feasible=%s ranked[0].viable=%s constraint=%s\n",
                sp.feasible ? "yes" : "no", sp.ranked[0].viable ? "yes" : "no",
                to_string(sp.ranked[0].constraint));
    for (const auto& n : sp.notes) std::printf("  note: %s\n", n.c_str());

    std::printf("\n== explain_backend (registry copy) ==\n");
    std::printf("  %s\n", sf.explain_backend(local).c_str());
    std::printf("  %s\n", sf.explain_backend(synth).c_str());
    std::printf("  note: set_health updates the live backend descriptor; the registry copy used by "
                "explain_backend reflects only registration-time state.\n");

    std::filesystem::remove_all(root, ec);
    std::printf("EX11_OK\n");
    return 0;
}
