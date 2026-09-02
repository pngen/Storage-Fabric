#include "storagefabric/core/planner.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <limits>

namespace storagefabric {

const char* to_string(PlannerConstraint v) noexcept {
    switch (v) {
        case PlannerConstraint::NONE: return "NONE";
        case PlannerConstraint::DURABILITY_UNAVAILABLE: return "DURABILITY_UNAVAILABLE";
        case PlannerConstraint::INSUFFICIENT_CAPACITY: return "INSUFFICIENT_CAPACITY";
        case PlannerConstraint::BACKEND_UNAVAILABLE: return "BACKEND_UNAVAILABLE";
        case PlannerConstraint::OBJECT_TOO_LARGE: return "OBJECT_TOO_LARGE";
        case PlannerConstraint::UNSUPPORTED_OBJECT_CLASS: return "UNSUPPORTED_OBJECT_CLASS";
        case PlannerConstraint::FAILURE_DOMAIN_DIVERSITY_IMPOSSIBLE: return "FAILURE_DOMAIN_DIVERSITY_IMPOSSIBLE";
    }
    return "UNKNOWN";
}

namespace {
struct CandidateScore {
    double score{0.0};
};

// Normalizes a latency into a 0..1 "goodness" (lower is better). Unknown -> 0.5.
double latency_goodness(double latency_s) {
    if (std::isnan(latency_s)) return 0.5;
    // Map 10us -> ~1.0, 1s -> ~0.0 logarithmically.
    const double base = std::log10(std::max(latency_s, 1e-9));
    const double hi = std::log10(1.0);
    const double lo = std::log10(1e-5);
    double g = 1.0 - (base - lo) / (hi - lo);
    return std::clamp(g, 0.0, 1.0);
}

double throughput_goodness(double bps) {
    if (std::isnan(bps)) return 0.5;
    if (bps <= 0) return 0.0;
    const double base = std::log10(bps);
    const double g = (base - 5.0) / 5.0;  // 100KB/s -> 0, ~10GB/s -> 1
    return std::clamp(g, 0.0, 1.0);
}

double cost_class_score(const std::string& cls) {
    // Coarse cost-class goodness: cheaper tiers score higher.
    if (cls == "local" || cls == "nvme") return 1.0;
    if (cls == "local-fs" || cls == "local_filesystem") return 0.9;
    if (cls == "cache") return 0.7;
    if (cls == "shared-fs") return 0.6;
    if (cls == "object") return 0.3;
    if (cls == "remote" || cls == "synthetic-remote") return 0.2;
    return 0.5;
}

bool is_available(const TierCandidate& c) { return c.health == Health::HEALTHY || c.health == Health::DEGRADED; }

bool capacity_ok(const TierCandidate& c, std::uint64_t size) {
    if (c.capacity_unknown) return true;   // unknown capacity cannot be proven insufficient
    return c.free_bytes >= size;
}
}  // namespace

StoragePlan plan(const PlanRequest& req) {
    StoragePlan plan;
    plan.object = req.object;
    plan.object_generation = req.object_generation;
    plan.policy_generation = req.policy_generation;

    // Hard rejection: unsupported object kind.
    const bool unsupported_kind =
        req.kind == ObjectKind::GENERIC_AI_STATE && req.logical_size == 0;
    if (unsupported_kind) {
        plan.notes.push_back("refusing to plan unsupported zero-size generic object");
    }

    const std::uint64_t size = req.logical_size;
    bool any_viable = false;
    for (const auto& c : req.candidates) {
        PlacementCandidate pc;
        pc.backend = c.backend;
        pc.tier = c.tier;

        // ---- hard constraints ----
        std::vector<std::string> hard;
        if (!is_available(c)) {
            pc.viable = false;
            pc.constraint = PlannerConstraint::BACKEND_UNAVAILABLE;
            hard.push_back("backend unavailable (" + std::string(to_string(c.health)) + ")");
        }
        if (pc.viable && req.max_object_bytes != 0 && size > req.max_object_bytes) {
            pc.viable = false;
            pc.constraint = PlannerConstraint::OBJECT_TOO_LARGE;
            hard.push_back("object exceeds backend max object size");
        }
        if (pc.viable && !capacity_ok(c, size)) {
            pc.viable = false;
            pc.constraint = PlannerConstraint::INSUFFICIENT_CAPACITY;
            hard.push_back("insufficient capacity");
        }
        if (pc.viable && is_available(c) && c.health == Health::DEGRADED) {
            // degraded considered but flagged
            pc.hard_violations.push_back("backend degraded (soft)");
        }
        // Durability requirement: a candidate is only durable if the tier is
        // persistent, or the failure domain supports it.
        if (pc.viable && req.required_replicas > 1 && !c.persistent) {
            pc.viable = false;
            pc.constraint = PlannerConstraint::DURABILITY_UNAVAILABLE;
            hard.push_back("tier not persistent for multi-replica durability");
        }

        // ---- named soft factors ----
        std::vector<PlanFactor> factors;
        const double locality_good = (req.locality_preference != StorageClass::UNKNOWN &&
                                      c.storage_class == req.locality_preference) ? 1.0 : 0.5;
        const double locality_weight = 0.25;
        factors.push_back({"locality", locality_good * locality_weight, locality_weight,
                           PlanConstraintKind::SOFT, static_cast<PlanFactorOrigin>(c.provenance),
                           "storage class matches locality preference"});
        const double lat_good = latency_goodness(c.read_latency_s);
        const double lat_weight = 0.25;
        factors.push_back({"read_latency", lat_good * lat_weight, lat_weight,
                           PlanConstraintKind::SOFT, static_cast<PlanFactorOrigin>(c.provenance),
                           "measured/synthetic read latency"});
        const double bw_good = throughput_goodness(c.write_bps);
        const double bw_weight = 0.20;
        factors.push_back({"write_bandwidth", bw_good * bw_weight, bw_weight,
                           PlanConstraintKind::SOFT, static_cast<PlanFactorOrigin>(c.provenance),
                           "write throughput"});
        const double cost_good = (c.cost_class.empty()) ? 0.5 : cost_class_score(c.cost_class);
        const double cost_weight = 0.15;
        factors.push_back({"cost", cost_good * cost_weight, cost_weight,
                           PlanConstraintKind::SOFT, PlanFactorOrigin::DERIVED,
                           "cost class"});
        // headroom factor: prefers more free space
        double headroom_good = 0.5;
        if (!c.capacity_unknown && c.total_bytes > 0) {
            const double free_ratio = static_cast<double>(c.free_bytes) / static_cast<double>(c.total_bytes);
            headroom_good = std::clamp(free_ratio, 0.0, 1.0);
        }
        const double headroom_weight = 0.15;
        factors.push_back({"headroom", headroom_good * headroom_weight, headroom_weight,
                           PlanConstraintKind::SOFT, static_cast<PlanFactorOrigin>(c.provenance),
                           "free capacity ratio"});

        double score = 0.0;
        for (const auto& f : factors) score += f.value;
        pc.factors = std::move(factors);
        pc.score = score;
        pc.hard_violations = hard;
        if (pc.viable) any_viable = true;
        plan.ranked.push_back(std::move(pc));
    }

    // Deterministic ordering: viable first, by score desc then backend id asc.
    std::stable_sort(plan.ranked.begin(), plan.ranked.end(),
        [](const PlacementCandidate& a, const PlacementCandidate& b) {
            if (a.viable != b.viable) return a.viable && !b.viable;
            if (a.score != b.score) return a.score > b.score;
            return a.backend.value() < b.backend.value();
        });

    plan.feasible = any_viable;
    if (any_viable) {
        plan.selected = plan.ranked.front();
    } else if (!plan.ranked.empty()) {
        plan.notes.push_back("no viable candidate; best is '" +
                             std::string(to_string(plan.ranked.front().constraint)) + "'");
    }

    // Replica targets: choose up to required distinct failure domains.
    if (any_viable && req.required_replicas > 1) {
        std::set<std::string> domains;
        for (const auto& c : req.candidates) {
            if (c.health != Health::HEALTHY && c.health != Health::DEGRADED) continue;
            if (domains.insert(c.failure_domain).second) {
                ReplicaPlan rp;
                rp.backend = c.backend;
                rp.tier = c.tier;
                rp.failure_domain = c.failure_domain;
                plan.replica_targets.push_back(std::move(rp));
            }
            if (plan.replica_targets.size() >= req.required_replicas) break;
        }
        if (plan.replica_targets.size() < req.required_replicas) {
            plan.notes.push_back("cannot satisfy required failure-domain diversity; target count=" +
                                 std::to_string(plan.replica_targets.size()));
        }
    }
    return plan;
}

}  // namespace storagefabric
