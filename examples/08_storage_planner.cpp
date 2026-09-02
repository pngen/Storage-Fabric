#include "storagefabric/core/planner.h"
#include <cstdio>
#include <vector>

using namespace storagefabric;

static TierCandidate make_candidate(StorageBackendId bid, StorageTierId tid, StorageClass cls,
                                    double free_b, double total_b, double lat, double wbps,
                                    Health health, const std::string& dom, const std::string& cost,
                                    MeasurementKind prov) {
    TierCandidate c;
    c.backend = bid;
    c.tier = tid;
    c.storage_class = cls;
    c.free_bytes = static_cast<std::uint64_t>(free_b);
    c.total_bytes = static_cast<std::uint64_t>(total_b);
    c.capacity_unknown = false;
    c.read_latency_s = lat;
    c.write_latency_s = lat;
    c.read_bps = wbps * 4.0;
    c.write_bps = wbps;
    c.health = health;
    c.eviction_capable = true;
    c.persistent = (cls == StorageClass::LOCAL_FILESYSTEM);
    c.failure_domain = dom;
    c.cost_class = cost;
    c.locality = cost;
    c.current_pressure = 0;
    c.provenance = prov;
    return c;
}

int main() {
    const std::uint64_t size = 64 * 1024;

    // Build a candidate pool: a fast local tier, a slower synthetic remote tier,
    // an unavailable backend, and a capacity-starved backend.
    std::vector<TierCandidate> candidates;
    candidates.push_back(make_candidate(StorageBackendId(1), StorageTierId(10), StorageClass::LOCAL_FILESYSTEM,
        8e11, 1e12, 1e-5, 1e9, Health::HEALTHY, "rack-a", "local", MeasurementKind::MEASURED));
    candidates.push_back(make_candidate(StorageBackendId(2), StorageTierId(20), StorageClass::SYNTHETIC_REMOTE,
        9e11, 1e12, 1e-3, 50.0 * 1024 * 1024, Health::HEALTHY, "rack-b", "synthetic-remote", MeasurementKind::SYNTHETIC));
    candidates.push_back(make_candidate(StorageBackendId(3), StorageTierId(30), StorageClass::LOCAL_FILESYSTEM,
        8e11, 1e12, 1e-5, 1e9, Health::UNAVAILABLE, "rack-c", "local", MeasurementKind::MEASURED));
    candidates.push_back(make_candidate(StorageBackendId(4), StorageTierId(40), StorageClass::LOCAL_FILESYSTEM,
        0.0, 1e12, 1e-5, 1e9, Health::HEALTHY, "rack-d", "local", MeasurementKind::MEASURED));

    PlanRequest req;
    req.object = ObjectId(100);
    req.object_generation = ObjectGeneration(1);
    req.kind = ObjectKind::CHECKPOINT;
    req.logical_size = size;
    req.required_replicas = 1;
    req.restore_priority = RestorePriority::HIGH;
    req.policy_generation = PolicyGeneration(1);
    req.candidates = candidates;

    const StoragePlan sp = plan(req);
    std::printf("== Deterministic planner: ranked candidates ==\n");
    for (const auto& c : sp.ranked) {
        std::printf("  backend=%s tier=%s viable=%s score=%.4f constraint=%s\n",
                    c.backend.str().c_str(), c.tier.str().c_str(),
                    c.viable ? "yes" : "no", c.score, to_string(c.constraint));
        for (const auto& f : c.factors) {
            std::printf("      factor '%s' value=%.4f weight=%.4f origin=%s\n",
                        f.name.c_str(), f.value, f.weight, to_string(f.origin));
        }
        if (!c.hard_violations.empty()) {
            for (const auto& hv : c.hard_violations) std::printf("      hard violation: %s\n", hv.c_str());
        }
    }
    std::printf("  plan feasible=%s\n", sp.feasible ? "yes" : "no");
    if (sp.selected) {
        std::printf("  selected backend=%s tier=%s (local measured tier requested)\n",
                    sp.selected->backend.str().c_str(), sp.selected->tier.str().c_str());
    }

    // Determinism: stable sort by (viable, score desc, backend id asc). Backends 1 and 2 both
    // viable; 1 has the better score so it wins. Non-viable 3 and 4 trail regardless of score.
    std::printf("\n== Determinism check ==\n");
    std::printf("  ranked[0]=backend%s ranked[1]=backend%s (local > synthetic)\n",
                sp.ranked[0].backend.str().c_str(), sp.ranked[1].backend.str().c_str());
    std::printf("  ranked[2]=backend%s (unavailable -> %s)\n",
                sp.ranked[2].backend.str().c_str(), to_string(sp.ranked[2].constraint));
    std::printf("  ranked[3]=backend%s (no capacity -> %s)\n",
                sp.ranked[3].backend.str().c_str(), to_string(sp.ranked[3].constraint));

    // Failure-domain diversity for a 2-replica durability requirement.
    PlanRequest r2 = req;
    r2.required_replicas = 2;
    r2.object_generation = ObjectGeneration(2);
    const StoragePlan sp2 = plan(r2);
    std::printf("\n== Replica target diversity (required=%u) ==\n", r2.required_replicas);
    std::printf("  replica_targets=%zu\n", sp2.replica_targets.size());
    for (const auto& t : sp2.replica_targets) {
        std::printf("    backend=%s tier=%s domain=%s\n", t.backend.str().c_str(), t.tier.str().c_str(), t.failure_domain.c_str());
    }
    for (const auto& n : sp2.notes) std::printf("  note: %s\n", n.c_str());

    // Hard constraint: an object larger than a backend's max object size is rejected.
    PlanRequest toobig;
    toobig.object = ObjectId(200);
    toobig.kind = ObjectKind::CHECKPOINT;
    toobig.logical_size = 8 * 1024 * 1024;   // 8 MiB
    toobig.required_replicas = 1;
    toobig.max_object_bytes = 1024;          // 1 KiB ceiling
    toobig.candidates = candidates;
    const StoragePlan spBig = plan(toobig);
    std::printf("\n== Hard constraint: object too large ==\n");
    for (const auto& c : spBig.ranked) {
        std::printf("  backend=%s viable=%s constraint=%s\n",
                    c.backend.str().c_str(), c.viable ? "yes" : "no", to_string(c.constraint));
    }
    std::printf("  plan feasible=%s\n", spBig.feasible ? "yes" : "no");

    // Hard constraint: a non-persistent tier cannot satisfy multi-replica durability.
    PlanRequest dur;
    dur.object = ObjectId(201);
    dur.kind = ObjectKind::CHECKPOINT;
    dur.logical_size = 64 * 1024;
    dur.required_replicas = 2;               // demands durability
    dur.candidates = candidates;
    const StoragePlan spDur = plan(dur);
    std::printf("\n== Hard constraint: durability on a non-persistent tier ==\n");
    for (const auto& c : spDur.ranked) {
        std::printf("  backend=%s viable=%s constraint=%s\n",
                    c.backend.str().c_str(), c.viable ? "yes" : "no", to_string(c.constraint));
        if (c.constraint == PlannerConstraint::DURABILITY_UNAVAILABLE)
            std::printf("    -> this candidate is not persistent, so it cannot be a durable replica target\n");
    }
    for (const auto& n : spDur.notes) std::printf("  note: %s\n", n.c_str());

    std::printf("EX08_OK\n");
    return 0;
}
