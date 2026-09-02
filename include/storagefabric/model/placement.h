#pragma once
// Storage Fabric - authoritative placement record.
// A placement is governed state. It only becomes AVAILABLE after integrity
// verification and commit. Lifecycle transitions are guarded.

#include <cstdint>
#include <string>

#include "storagefabric/core/strong.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/model/enums.h"

namespace storagefabric {

struct PlacementRecord {
    PlacementId id;
    ObjectId object;
    ObjectGeneration object_generation;
    ReplicaId replica;
    ReplicaGeneration replica_generation;
    StorageBackendId backend;
    StorageTierId tier;
    VolumeId volume;
    std::string key;                 // backend-relative governed object key
    ManifestId manifest;
    std::uint64_t logical_size{0};
    std::uint64_t physical_size{0};
    ContentDigest digest;
    std::uint32_t durability_replicas{0};
    std::string locality;
    PlacementLifecycle lifecycle{PlacementLifecycle::PLANNED};
    Freshness freshness{Freshness::UNKNOWN};
    AuthorityGeneration authority_generation;
    AuthorityOrigin provenance{AuthorityOrigin::UNKNOWN};
    WorkerId writer_worker;
    WorkerBootId writer_boot;
    PlacementGeneration placement_generation;
    std::int64_t created_at_ms{0};
    std::int64_t available_at_ms{0};

    bool is_authoritative() const noexcept { return lifecycle == PlacementLifecycle::AVAILABLE; }
    bool is_active() const noexcept {
        return lifecycle == PlacementLifecycle::AVAILABLE || lifecycle == PlacementLifecycle::DEGRADED;
    }
    bool digest_verified() const noexcept { return !digest.is_zero(); }

    // Guarded transition table. Only valid transitions are permitted.
    static bool is_valid_transition(PlacementLifecycle from, PlacementLifecycle to) noexcept;
};

// Transition guard: returns false (and sets an outcome) for an illegal move.
bool can_transition(PlacementLifecycle from, PlacementLifecycle to) noexcept;

}  // namespace storagefabric
