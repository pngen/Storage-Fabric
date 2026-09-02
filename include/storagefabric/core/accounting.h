#pragma once
// Storage Fabric - exact accounting counters.
// All counters are monotonic non-negative. Dedup/refcounts are exact. Duplicate
// completion/release may never double-account.

#include <cstdint>
#include <string>

namespace storagefabric {

struct AccountingTotals {
    // Objects and metadata
    std::uint64_t logical_objects{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t physical_blobs{0};
    std::uint64_t physical_bytes{0};
    std::uint64_t deduplicated_bytes{0};
    std::uint64_t chunks{0};
    std::uint64_t manifests{0};
    std::uint64_t active_placements{0};
    std::uint64_t replicas{0};
    std::uint64_t reserved_bytes{0};
    std::uint64_t committed_bytes{0};

    // Activity counters
    std::uint64_t active_writes{0};
    std::uint64_t active_reads{0};
    std::uint64_t transferred_bytes{0};
    std::uint64_t restored_bytes{0};
    std::uint64_t evicted_bytes{0};

    // Failure / rejection counters
    std::uint64_t integrity_failures{0};
    std::uint64_t stale_rejections{0};
    std::uint64_t duplicate_rejections{0};
    std::uint64_t backend_failures{0};
    std::uint64_t participant_restarts{0};

    bool is_consistent() const noexcept {
        return deduplicated_bytes <= physical_bytes;
    }
};

struct RefCountState {
    std::uint64_t refcount{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t physical_bytes{0};
};

}  // namespace storagefabric
