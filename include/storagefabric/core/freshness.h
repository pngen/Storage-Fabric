#pragma once
// Storage Fabric - freshness and health observability.
// Physical storage observations do not remain CURRENT indefinitely across a
// coordinator restart: they become REVALIDATION_REQUIRED. Logical object
// identity/digest may remain valid if re-verified.

#include <cstdint>

#include "storagefabric/core/strong.h"
#include "storagefabric/model/enums.h"

namespace storagefabric {

// Decides whether an observation made at 'at_ms' and valid for 'max_age_ms' is
// still CURRENT relative to 'now_ms'. A non-positive max age means "never goes
// stale on age" (only invalidated explicitly).
inline Freshness age_freshness(std::int64_t now_ms, std::int64_t at_ms, std::int64_t max_age_ms) noexcept {
    if (max_age_ms <= 0) return Freshness::CURRENT;
    if (now_ms - at_ms <= max_age_ms) return Freshness::CURRENT;
    return Freshness::STALE;
}

// On coordinator recovery, physical observations are downgraded to
// REVALIDATION_REQUIRED until a live probe re-establishes them.
inline Freshness recovery_freshness() noexcept {
    return Freshness::REVALIDATION_REQUIRED;
}

// Health retention: never classify a permanent physical failure from a single
// transient error. Requires a generation and a count of consecutive failures.
struct HealthAccumulator {
    std::uint32_t consecutive_failures{0};
    std::uint32_t failure_threshold{3};   // transient failures tolerated before DEGRADED
    Health current{Health::HEALTHY};
    HealthGeneration generation;

    Health record_io_error() noexcept {
        ++consecutive_failures;
        if (consecutive_failures >= failure_threshold) {
            current = Health::DEGRADED;
        }
        return current;
    }
    Health record_success() noexcept {
        consecutive_failures = 0;
        current = Health::HEALTHY;
        generation = generation.next();
        return current;
    }
    Health record_unavailable() noexcept {
        current = Health::UNAVAILABLE;
        generation = generation.next();
        return current;
    }
};

}  // namespace storagefabric
