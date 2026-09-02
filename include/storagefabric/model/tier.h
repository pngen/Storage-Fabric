#pragma once
// Storage Fabric - storage tier descriptors.
// A tier is a class of storage with a set of measured/reported/synthetic
// properties. Unknown properties remain UNKNOWN. Physical device types are
// never asserted unless proven by detection or measurement.

#include <cstdint>
#include <string>
#include <optional>

#include "storagefabric/core/strong.h"
#include "storagefabric/model/enums.h"

namespace storagefabric {

// A numeric observation holding both the value and how it was obtained.
template <typename T>
struct TypedObservation {
    T value{};
    MeasurementKind origin{MeasurementKind::UNKNOWN};
    std::uint64_t observed_at_ms{0};

    bool is_known() const noexcept { return origin != MeasurementKind::UNKNOWN; }
};

using BytesObservation = TypedObservation<std::uint64_t>;   // capacity bytes
using FixtureTimeObservation = TypedObservation<double>;     // seconds/op
using RateObservation = TypedObservation<double>;            // bytes/sec
using CountObservation = TypedObservation<std::uint32_t>;    // concurrency

// Coarse durability class a tier can provide (independent of a measurement).
enum class TierDurabilityClass : std::uint8_t {
    EPHEMERAL,
    LOCAL,
    REDUNDANT,
    UNKNOWN,
};

const char* to_string(TierDurabilityClass) noexcept;

struct StorageTier {
    StorageTierId id;
    StorageClass storage_class{StorageClass::UNKNOWN};
    std::string name;
    std::string failure_domain;          // e.g. "local", "rack-a", "node-1"
    std::string locality;                // free-form locality tag
    std::uint32_t concurrency{1};        // observed concurrency capability

    TierDurabilityClass durability_class{TierDurabilityClass::UNKNOWN};
    bool eviction_capable{false};
    bool persistent{false};

    // Capacity observations.
    BytesObservation nominal_capacity_bytes;
    BytesObservation measured_free_bytes;
    RateObservation write_throughput_bps;
    RateObservation read_throughput_bps;
    FixtureTimeObservation write_latency_s;
    FixtureTimeObservation read_latency_s;

    std::string cost_class;
    Freshness freshness{Freshness::UNKNOWN};
    Health health{Health::UNKNOWN};

    bool has_known_capacity() const noexcept { return nominal_capacity_bytes.is_known(); }
    bool is_eviction_capable() const noexcept { return eviction_capable; }
    bool is_persistent() const noexcept { return persistent; }
};

// Capacity accounting state for a specific backend.
struct BackendCapacity {
    std::uint64_t total_bytes{0};
    std::uint64_t free_bytes{0};        // nominal free
    std::uint64_t reserved_bytes{0};    // held by active reservations
    std::uint64_t committed_bytes{0};   // committed by published placements
    std::uint64_t reclaimable_bytes{0};
    bool unknown{false};                // true when total/free are not probeable

    std::uint64_t available_bytes() const noexcept {
        const std::uint64_t considered_total =
            (total_bytes > reserved_bytes) ? (total_bytes - reserved_bytes) : 0;
        return considered_total;
    }
};

}  // namespace storagefabric
