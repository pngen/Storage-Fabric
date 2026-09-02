#pragma once
// Storage Fabric - replica set and replica membership.
// A stale replica must never become authoritative solely because bytes still
// exist. Rebuild produces a fresh ReplicaGeneration/PlacementGeneration.

#include <cstdint>
#include <vector>
#include <string>

#include "storagefabric/core/strong.h"
#include "storagefabric/model/enums.h"

namespace storagefabric {

struct ReplicaInfo {
    ReplicaId id;
    ReplicaGeneration generation;
    StorageBackendId backend;
    StorageNodeId node;
    std::string failure_domain;
    std::uint64_t logical_size{0};
    std::uint64_t physical_size{0};
    ReplicaState state{ReplicaState::UNDER_REPLICATED};
    PlacementGeneration placement_generation;
    bool authoritative{false};
};

struct ReplicaSet {
    ObjectId object;
    ObjectGeneration current_generation;
    std::uint32_t required{1};
    std::uint32_t actual{0};
    ReplicaState state{ReplicaState::UNDER_REPLICATED};
    std::uint32_t distinct_failure_domains{0};
    std::uint32_t authoritative_replicas{0};
    AuthorityGeneration authority_generation;
    std::vector<ReplicaInfo> replicas;
    std::int64_t updated_at_ms{0};

    bool meets_durability() const noexcept { return authoritative_replicas >= required; }
    ReplicaState compute_state() const;
};

}  // namespace storagefabric
