#pragma once
// Storage Fabric - deterministic placement planner.
// Uses named factors and hard constraints, never a single unexplained master
// score. Hard constraints reject a candidate outright; soft factors weight it.
// Tie-breaking is deterministic by (score, backend id).

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "storagefabric/core/strong.h"
#include "storagefabric/model/enums.h"

namespace storagefabric {

struct TierCandidate {
    StorageBackendId backend;
    StorageTierId tier;
    StorageClass storage_class{StorageClass::UNKNOWN};
    std::uint64_t free_bytes{0};
    std::uint64_t total_bytes{0};
    bool capacity_unknown{true};
    double read_latency_s{NAN};
    double write_latency_s{NAN};
    double read_bps{NAN};
    double write_bps{NAN};
    Health health{Health::UNKNOWN};
    bool eviction_capable{false};
    bool persistent{false};
    std::string failure_domain;
    std::string cost_class;
    std::string locality;
    std::uint64_t current_pressure{0};
    MeasurementKind provenance{MeasurementKind::UNKNOWN};
};

enum class PlannerConstraint {
    NONE,
    DURABILITY_UNAVAILABLE,
    INSUFFICIENT_CAPACITY,
    BACKEND_UNAVAILABLE,
    OBJECT_TOO_LARGE,
    UNSUPPORTED_OBJECT_CLASS,
    FAILURE_DOMAIN_DIVERSITY_IMPOSSIBLE,
};

const char* to_string(PlannerConstraint) noexcept;

struct PlanFactor {
    std::string name;
    double value{0.0};             // normalized contribution to score
    double weight{0.0};            // nominal weight
    PlanConstraintKind kind{PlanConstraintKind::SOFT};
    PlanFactorOrigin origin{PlanFactorOrigin::UNKNOWN};
    std::string evidence;
};

struct PlacementCandidate {
    StorageBackendId backend;
    StorageTierId tier;
    double score{0.0};
    bool viable{true};
    PlannerConstraint constraint{PlannerConstraint::NONE};
    std::vector<PlanFactor> factors;
    std::vector<std::string> hard_violations;
};

struct ReplicaPlan {
    StorageBackendId backend;
    StorageTierId tier;
    std::string failure_domain;
};

struct StoragePlan {
    ObjectId object;
    ObjectGeneration object_generation;
    bool feasible{false};
    std::vector<PlacementCandidate> ranked;
    std::optional<PlacementCandidate> selected;
    std::vector<ReplicaPlan> replica_targets;
    std::vector<std::string> notes;
    PolicyGeneration policy_generation;
};

struct PlanRequest {
    ObjectId object;
    ObjectGeneration object_generation;
    ObjectKind kind{ObjectKind::GENERIC_AI_STATE};
    std::uint64_t logical_size{0};
    std::uint32_t required_replicas{1};
    RestorePriority restore_priority{RestorePriority::NORMAL};
    StorageClass locality_preference{StorageClass::UNKNOWN};
    std::string locality_hint;
    PolicyGeneration policy_generation;
    std::vector<TierCandidate> candidates;
    std::uint64_t max_object_bytes{0};   // 0 means no limit
};

StoragePlan plan(const PlanRequest& req);

}  // namespace storagefabric
