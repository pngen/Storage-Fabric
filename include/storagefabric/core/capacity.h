#pragma once
// Storage Fabric - atomic capacity reservations and per-backend accounting.
// Rejects overcommit, duplicate reservation, stale release, double release, and
// any accounting that would go negative.

#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <unordered_map>
#include <memory>

#include "storagefabric/core/strong.h"
#include "storagefabric/core/status.h"
#include "storagefabric/model/tier.h"

namespace storagefabric {

struct Reservation {
    ReservationId id;
    ReservationGeneration generation;
    StorageBackendId backend;
    std::uint64_t bytes{0};
    ReservationState state{ReservationState::ACTIVE};
    WorkerId owner;
    std::string reason;
    std::int64_t created_at_ms{0};
    PlacementId bound_placement;   // set on commit
    bool committed{false};
};

class ReservationLedger {
public:
    explicit ReservationLedger(std::uint64_t overcommit_allowance = 0)
        : overcommit_allowance_(overcommit_allowance) {}

    // Registers the base capacity for a backend.
    void register_backend(StorageBackendId backend, const BackendCapacity& cap);
    // Refreshes the base free capacity from a live probe (keeps reserved/committed).
    void refresh_free(StorageBackendId backend, std::uint64_t free_bytes);

    Result<Reservation> reserve(StorageBackendId backend, std::uint64_t bytes,
                                ReservationGeneration gen, WorkerId owner,
                                std::string reason = {});

    // Converts a reservation into committed placement bytes.
    Status commit(ReservationId id, PlacementId placement);

    // Releases a reservation. Rejects stale generation and double release.
    Status release(ReservationId id, ReservationGeneration gen);

    // Marks a reported committed placement without a prior reservation.
    Status add_committed(StorageBackendId backend, std::uint64_t bytes);
    Status remove_committed(StorageBackendId backend, std::uint64_t bytes);

    std::uint64_t reserved(StorageBackendId backend) const;
    std::uint64_t committed(StorageBackendId backend) const;
    std::uint64_t free(StorageBackendId backend) const;
    std::uint64_t reclaimable(StorageBackendId backend) const;
    Result<BackendCapacity> snapshot(StorageBackendId backend) const;

    std::uint64_t total_reserved() const noexcept { return total_reserved_; }
    std::uint64_t total_committed() const noexcept { return total_committed_; }

private:
    struct PerBackend {
        BackendCapacity capacity;
    };

    mutable std::mutex mutex_;
    std::unordered_map<StorageBackendId, PerBackend> backends_;
    std::unordered_map<ReservationId, Reservation> reservations_;
    std::uint64_t overcommit_allowance_{0};
    std::uint64_t total_reserved_{0};
    std::uint64_t total_committed_{0};
    ReservationId next_reservation_id_{ReservationId(1)};
};

// A RAII reservation guard that releases on destruction unless committed.
class ReservationGuard {
public:
    ReservationGuard() = default;
    ReservationGuard(ReservationLedger* ledger, Reservation res, bool armed = true);
    ~ReservationGuard();
    ReservationGuard(const ReservationGuard&) = delete;
    ReservationGuard& operator=(const ReservationGuard&) = delete;
    ReservationGuard(ReservationGuard&& other) noexcept;
    ReservationGuard& operator=(ReservationGuard&& other) noexcept;

    Status commit(PlacementId placement);
    Status release();
    const Reservation& reservation() const noexcept { return res_; }
    bool is_armed() const noexcept { return armed_; }

private:
    void disarm() noexcept { armed_ = false; }
    ReservationLedger* ledger_{nullptr};
    Reservation res_;
    bool armed_{false};
};

}  // namespace storagefabric
