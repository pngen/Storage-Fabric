#include "test_util.h"
#include "storagefabric/core/planner.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/synthetic_backend.h"

#include <string>
#include <vector>

using namespace storagefabric;

// A healthy, generous local-ish candidate used as a baseline.
static TierCandidate base_candidate(StorageBackendId id, std::string domain) {
    TierCandidate c;
    c.backend = id;
    c.tier = StorageTierId(1);
    c.storage_class = StorageClass::LOCAL_FILESYSTEM;
    c.free_bytes = 1u << 30;
    c.total_bytes = 2u << 30;
    c.capacity_unknown = false;
    c.read_latency_s = 1e-4;
    c.write_latency_s = 1e-4;
    c.read_bps = 1e9;
    c.write_bps = 1e9;
    c.health = Health::HEALTHY;
    c.eviction_capable = true;
    c.persistent = true;
    c.failure_domain = domain;
    c.cost_class = "local";
    c.locality = "local";
    c.current_pressure = 0;
    c.provenance = MeasurementKind::MEASURED;
    return c;
}

static PlanRequest base_request(std::uint32_t replicas = 1) {
    PlanRequest req;
    req.object = ObjectId(1);
    req.object_generation = ObjectGeneration(1);
    req.kind = ObjectKind::CHECKPOINT;
    req.logical_size = 1024;
    req.required_replicas = replicas;
    req.policy_generation = PolicyGeneration(1);
    return req;
}

static bool has_factor(const PlacementCandidate& pc, const std::string& name) {
    for (const auto& f : pc.factors) if (f.name == name) return true;
    return false;
}

// Registers a synthetic backend on a fresh runtime and returns its id.
static StorageBackendId synth(StorageFabric& sf, const std::string& name,
                              StorageClass cls, std::uint64_t total, std::uint64_t free,
                              double write_bps, const std::string& domain,
                              const std::string& cost, Health health = Health::HEALTHY) {
    SyntheticProfile p;
    p.storage_class = cls;
    p.total_bytes = total;
    p.free_bytes = free;
    p.read_bps = write_bps;
    p.write_bps = write_bps;
    p.health = health;
    p.degraded = (health == Health::DEGRADED);
    p.unavailable = (health == Health::UNAVAILABLE);
    p.evictable = false;
    p.persistent = true;
    p.failure_domain = domain;
    p.cost_class = cost;
    const auto r = sf.register_synthetic_backend(name, p);
    CHECK_OK(r);
    return r.value();
}

// Defines a small object on the runtime and returns its descriptor.
static ObjectDescriptor make_obj(StorageFabric& sf, const AuthorityEnvelope& auth,
                                 RestorePriority prio = RestorePriority::NORMAL) {
    const std::vector<std::uint8_t> content(1024, 0xAB);
    ByteSpan span = ByteSpan(content.data(), content.size());
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur, prio);
    CHECK_OK(obj);
    return obj.value();
}

int main() {
    std::printf("test_planner starting\n");

    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(100);
    auth.worker = WorkerId(7);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;

    // ---- named factors present with provenance ----
    {
        PlanRequest req = base_request();
        req.candidates = {base_candidate(StorageBackendId(1), "rack-a")};
        StoragePlan sp = plan(req);
        CHECK(sp.feasible);
        CHECK(sp.selected.has_value());
        const PlacementCandidate& sel = sp.selected.value();
        CHECK(has_factor(sel, "locality"));
        CHECK(has_factor(sel, "read_latency"));
        CHECK(has_factor(sel, "write_bandwidth"));
        CHECK(has_factor(sel, "cost"));
        CHECK(has_factor(sel, "headroom"));
        for (const auto& f : sel.factors) {
            CHECK(!f.name.empty());
            CHECK(f.weight > 0.0);
            if (f.name != "cost") {
                CHECK_EQ(static_cast<int>(f.origin), static_cast<int>(PlanFactorOrigin::MEASURED));
            } else {
                CHECK_EQ(static_cast<int>(f.origin), static_cast<int>(PlanFactorOrigin::DERIVED));
            }
            CHECK(!f.evidence.empty());
        }
        std::printf("  named factors present with provenance PASS\n");
    }

    // ---- hard constraints ----
    {
        PlanRequest req = base_request();
        TierCandidate small = base_candidate(StorageBackendId(1), "rack-a");
        small.free_bytes = 100;
        req.candidates = {small};
        StoragePlan sp = plan(req);
        CHECK(!sp.feasible);
        CHECK(!sp.ranked.empty());
        CHECK(!sp.ranked[0].viable);
        CHECK_EQ(static_cast<int>(sp.ranked[0].constraint),
                 static_cast<int>(PlannerConstraint::INSUFFICIENT_CAPACITY));

        PlanRequest req2 = base_request();
        TierCandidate down = base_candidate(StorageBackendId(1), "rack-a");
        down.health = Health::UNAVAILABLE;
        req2.candidates = {down};
        StoragePlan sp2 = plan(req2);
        CHECK(!sp2.feasible);
        CHECK_EQ(static_cast<int>(sp2.ranked[0].constraint),
                 static_cast<int>(PlannerConstraint::BACKEND_UNAVAILABLE));

        PlanRequest req3 = base_request();
        req3.max_object_bytes = 128;
        req3.candidates = {base_candidate(StorageBackendId(1), "rack-a")};
        StoragePlan sp3 = plan(req3);
        CHECK(!sp3.feasible);
        CHECK_EQ(static_cast<int>(sp3.ranked[0].constraint),
                 static_cast<int>(PlannerConstraint::OBJECT_TOO_LARGE));

        PlanRequest req4 = base_request(2);
        TierCandidate nonpersist = base_candidate(StorageBackendId(1), "rack-a");
        nonpersist.persistent = false;
        req4.candidates = {nonpersist};
        StoragePlan sp4 = plan(req4);
        CHECK(!sp4.feasible);
        CHECK_EQ(static_cast<int>(sp4.ranked[0].constraint),
                 static_cast<int>(PlannerConstraint::DURABILITY_UNAVAILABLE));
        std::printf("  hard constraints PASS\n");
    }

    // ---- deterministic tie-break ----
    {
        PlanRequest req = base_request();
        TierCandidate a = base_candidate(StorageBackendId(1), "rack-a");
        TierCandidate b = base_candidate(StorageBackendId(2), "rack-a");
        req.candidates = {a, b};
        StoragePlan sp = plan(req);
        CHECK(sp.feasible);
        CHECK_EQ(sp.ranked.size(), 2u);
        CHECK(sp.ranked[0].backend == StorageBackendId(1));
        CHECK(sp.ranked[1].backend == StorageBackendId(2));

        TierCandidate big = base_candidate(StorageBackendId(1), "rack-a");
        big.free_bytes = 100;
        TierCandidate ok = base_candidate(StorageBackendId(2), "rack-a");
        req.candidates = {big, ok};
        StoragePlan sp2 = plan(req);
        CHECK(sp2.feasible);
        CHECK(sp2.ranked[0].backend == StorageBackendId(2));
        CHECK(sp2.ranked[0].viable);
        CHECK(!sp2.ranked[1].viable);
        std::printf("  deterministic tie-break PASS\n");
    }

    // ---- synthetic scenarios, each isolated on a fresh runtime ----
    {
        // degraded is still considered available (runs, not rejected).
        StorageFabric sf;
        sf.set_authority(auth);
        auto deg = synth(sf, "degraded", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e8,
                         "rack-a", "synthetic-remote", Health::DEGRADED);
        auto obj = make_obj(sf, auth);
        StoragePlan sp = sf.plan(sf.make_plan_request(obj, 1));
        CHECK(sp.feasible);
        CHECK(sp.selected.has_value());
        CHECK(sp.selected.value().backend == deg);
        std::printf("  synthetic: degraded is still available PASS\n");
    }
    {
        // unavailable
        StorageFabric sf;
        sf.set_authority(auth);
        (void)synth(sf, "down", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e8,
                    "rack-a", "synthetic-remote", Health::UNAVAILABLE);
        auto obj = make_obj(sf, auth);
        StoragePlan sp = sf.plan(sf.make_plan_request(obj, 1));
        CHECK(!sp.feasible);
        bool uncon = false;
        for (const auto& n : sp.notes)
            if (n.find("BACKEND_UNAVAILABLE") != std::string::npos) uncon = true;
        CHECK(uncon);
        std::printf("  synthetic: unavailable PASS\n");
    }
    {
        // asymmetric (bandwidth split): higher-write-bw tier wins
        StorageFabric sf;
        sf.set_authority(auth);
        auto hi = synth(sf, "hi-bw", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 8e7,
                        "rack-a", "synthetic-remote");
        (void)synth(sf, "lo-bw", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e7,
                    "rack-b", "synthetic-remote");
        auto obj = make_obj(sf, auth);
        StoragePlan sp = sf.plan(sf.make_plan_request(obj, 1));
        CHECK(sp.feasible);
        CHECK(sp.selected.has_value());
        CHECK(sp.selected.value().backend == hi);
        std::printf("  synthetic: asymmetric (higher-bw tier wins) PASS\n");
    }
    {
        // high-bw-low-cap vs low-bw-high-cap
        StorageFabric sf;
        sf.set_authority(auth);
        auto hb_lc = synth(sf, "hb-lc", StorageClass::SYNTHETIC_REMOTE, 1000, 100, 1e9,
                           "rack-a", "synthetic-remote");
        auto lb_hc = synth(sf, "lb-hc", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e7,
                           "rack-b", "synthetic-remote");
        auto obj = make_obj(sf, auth);
        StoragePlan sp = sf.plan(sf.make_plan_request(obj, 1));
        CHECK(sp.feasible);
        CHECK(sp.selected.has_value());
        CHECK(sp.selected.value().backend == lb_hc);
        bool capped = false;
        for (const auto& c : sp.ranked)
            if (c.backend == hb_lc)
                capped = (c.constraint == PlannerConstraint::INSUFFICIENT_CAPACITY);
        CHECK(capped);
        std::printf("  synthetic: high-bw-low-cap / low-bw-high-cap PASS\n");
    }
    {
        // under-replicated (single failure domain)
        StorageFabric sf;
        sf.set_authority(auth);
        (void)synth(sf, "u1", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e8,
                    "node-1", "synthetic-remote");
        (void)synth(sf, "u2", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e8,
                    "node-1", "synthetic-remote");
        auto obj = make_obj(sf, auth);
        StoragePlan sp = sf.plan(sf.make_plan_request(obj, 2));
        CHECK(sp.feasible);
        bool undnote = false;
        for (const auto& n : sp.notes)
            if (n.find("failure-domain diversity") != std::string::npos) undnote = true;
        CHECK(undnote);
        CHECK(sp.replica_targets.size() <= 2u);
        std::printf("  synthetic: under-replicated diversity note PASS\n");
    }
    {
        // restore-priority carried into the plan (no conflict for a durable tier)
        StorageFabric sf;
        sf.set_authority(auth);
        (void)synth(sf, "pk", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e8,
                    "rack-a", "synthetic-remote");
        auto obj = make_obj(sf, auth, RestorePriority::CRITICAL);
        auto req = sf.make_plan_request(obj, 1);
        CHECK_EQ(static_cast<int>(req.restore_priority), static_cast<int>(RestorePriority::CRITICAL));
        StoragePlan sp = sf.plan(req);
        CHECK(sp.feasible);
        std::printf("  synthetic: restore-priority carried into plan PASS\n");
    }
    {
        // eviction pressure (headroom discriminator)
        StorageFabric sf;
        sf.set_authority(auth);
        auto p_a = synth(sf, "p-a", StorageClass::SYNTHETIC_REMOTE, 100000, 90000, 1e8,
                         "rack-a", "synthetic-remote");
        (void)synth(sf, "p-b", StorageClass::SYNTHETIC_REMOTE, 100000, 10000, 1e8,
                    "rack-b", "synthetic-remote");
        auto obj = make_obj(sf, auth);
        StoragePlan sp = sf.plan(sf.make_plan_request(obj, 1));
        CHECK(sp.feasible);
        CHECK(sp.selected.has_value());
        CHECK(sp.selected.value().backend == p_a);
        std::printf("  synthetic: eviction pressure / headroom PASS\n");
    }
    {
        // movement after policy change
        StorageFabric sf;
        sf.set_authority(auth);
        auto nvme = synth(sf, "nvme", StorageClass::LOCAL_NVME, 1u << 30, 1u << 30, 1e8,
                          "rack-a", "nvme");
        auto rem = synth(sf, "remote", StorageClass::SYNTHETIC_REMOTE, 1u << 30, 1u << 30, 1e8,
                         "rack-b", "synthetic-remote");
        auto obj = make_obj(sf, auth);

        ObjectDescriptor d_nvme = obj;
        d_nvme.locality.preferred = StorageClass::LOCAL_NVME;
        StoragePlan sp1 = sf.plan(sf.make_plan_request(d_nvme, 1));
        CHECK(sp1.feasible);
        CHECK(sp1.selected.has_value());
        CHECK(sp1.selected.value().backend == nvme);

        ObjectDescriptor d_rem = obj;
        d_rem.locality.preferred = StorageClass::SYNTHETIC_REMOTE;
        StoragePlan sp2 = sf.plan(sf.make_plan_request(d_rem, 1));
        CHECK(sp2.feasible);
        CHECK(sp2.selected.has_value());
        CHECK(sp2.selected.value().backend == rem);
        std::printf("  synthetic: movement after policy change PASS\n");
    }

    std::printf("test_planner: ALL PASS\n");
    return 0;
}
