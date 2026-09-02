#pragma once
// Storage Fabric - backend descriptor and health.
#include <cstdint>
#include <string>
#include <vector>

#include "storagefabric/core/strong.h"
#include "storagefabric/model/enums.h"
#include "storagefabric/model/tier.h"

namespace storagefabric {

enum class DurabilityBacked : std::uint8_t { NONE, LOCAL, REDUNDANT, UNKNOWN };

struct BackendDescriptor {
    StorageBackendId id;
    StorageTierId tier;
    StorageNodeId node;
    std::string name;
    BackendGeneration generation;
    BackendCapacity capacity;
    std::vector<BackendCapability> capabilities;
    Health health{Health::UNKNOWN};
    Freshness freshness{Freshness::UNKNOWN};
    MeasurementKind provenance{MeasurementKind::UNKNOWN};    // SYNTHETIC for simulated tiers
    std::int64_t registered_at_ms{0};

    bool has_capability(BackendCapability c) const noexcept {
        for (const auto cap : capabilities) {
            if (cap == c) return true;
        }
        return false;
    }
    bool is_available() const noexcept { return health == Health::HEALTHY || health == Health::DEGRADED; }
    bool is_synthetic() const noexcept { return provenance == MeasurementKind::SYNTHETIC; }
};

// A recorded health observation with generation fencing.
struct HealthObservation {
    ObservationId id;
    BackendGeneration health_generation;
    Health health{Health::UNKNOWN};
    std::string reason;
    std::int64_t at_ms{0};
};

// Map from backend id to health state used during planning.
struct BackendHealthReport {
    StorageBackendId backend;
    Health health{Health::UNKNOWN};
    HealthGeneration health_gen;
    std::string evidence;
};

}  // namespace storagefabric
