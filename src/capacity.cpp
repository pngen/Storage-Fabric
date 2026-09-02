#include "storagefabric/core/capacity.h"

#include <algorithm>

namespace storagefabric {

void ReservationLedger::register_backend(StorageBackendId backend, const BackendCapacity& cap) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto& b = backends_[backend];
    b.capacity = cap;
}

void ReservationLedger::refresh_free(StorageBackendId backend, std::uint64_t free_bytes) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it != backends_.end()) it->second.capacity.free_bytes = free_bytes;
}

Result<Reservation> ReservationLedger::reserve(StorageBackendId backend, std::uint64_t bytes,
                                               ReservationGeneration gen, WorkerId owner,
                                               std::string reason) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto bit = backends_.find(backend);
    if (bit == backends_.end()) {
        return Result<Reservation>::failure(StatusCode::NotFound, "backend not registered in ledger");
    }
    PerBackend& b = bit->second;
    if (bytes == 0) return Result<Reservation>::failure(StatusCode::InvalidArgument, "zero-byte reservation");

    const std::uint64_t occupancy = b.capacity.reserved_bytes + b.capacity.committed_bytes;
    const std::uint64_t headroom =
        (b.capacity.free_bytes + overcommit_allowance_ > occupancy)
            ? (b.capacity.free_bytes + overcommit_allowance_ - occupancy)
            : 0;
    if (bytes > headroom) {
        return Result<Reservation>::failure(StatusCode::InsufficientCapacity,
                                            "reservation would overcommit capacity");
    }

    Reservation res;
    res.id = next_reservation_id_;
    next_reservation_id_ = next_reservation_id_.next();
    res.generation = gen;
    res.backend = backend;
    res.bytes = bytes;
    res.state = ReservationState::ACTIVE;
    res.owner = owner;
    res.reason = std::move(reason);
    res.created_at_ms = 0;
    if (reservations_.count(res.id)) {
        return Result<Reservation>::failure(StatusCode::DuplicateReservation, "duplicate reservation id");
    }
    reservations_.emplace(res.id, res);
    b.capacity.reserved_bytes += bytes;
    total_reserved_ += bytes;
    return res;
}

Status ReservationLedger::commit(ReservationId id, PlacementId placement) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = reservations_.find(id);
    if (it == reservations_.end()) return Status(StatusCode::NotFound, "reservation not found");
    Reservation& res = it->second;
    if (res.state != ReservationState::ACTIVE) {
        return Status(StatusCode::InvalidState, "reservation is not active; cannot commit");
    }
    auto bit = backends_.find(res.backend);
    if (bit == backends_.end()) return Status(StatusCode::NotFound, "backend not registered");
    PerBackend& b = bit->second;
    if (b.capacity.reserved_bytes < res.bytes) {
        return Status(StatusCode::NegativeAccounting, "reserved accounting underflow");
    }
    b.capacity.reserved_bytes -= res.bytes;
    b.capacity.committed_bytes += res.bytes;
    total_reserved_ -= res.bytes;
    total_committed_ += res.bytes;
    res.state = ReservationState::COMMITTED;
    res.committed = true;
    res.bound_placement = placement;
    return Status::ok_status();
}

Status ReservationLedger::release(ReservationId id, ReservationGeneration gen) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = reservations_.find(id);
    if (it == reservations_.end()) {
        return Status(StatusCode::StaleReservation, "unknown reservation (already released or stale)");
    }
    Reservation& res = it->second;
    if (res.state == ReservationState::RELEASED || res.state == ReservationState::EXPIRED) {
        return Status(StatusCode::DuplicateReservation, "double release of reservation");
    }
    if (gen.value() != res.generation.value()) {
        return Status(StatusCode::StaleReservation, "stale reservation generation");
    }
    if (res.committed) {
        return Status(StatusCode::InvalidState, "reservation already committed; cannot release");
    }
    auto bit = backends_.find(res.backend);
    if (bit == backends_.end()) return Status(StatusCode::NotFound, "backend not registered");
    PerBackend& b = bit->second;
    if (b.capacity.reserved_bytes < res.bytes) {
        return Status(StatusCode::NegativeAccounting, "reserved accounting underflow");
    }
    b.capacity.reserved_bytes -= res.bytes;
    total_reserved_ -= res.bytes;
    res.state = ReservationState::RELEASED;
    return Status::ok_status();
}

Status ReservationLedger::add_committed(StorageBackendId backend, std::uint64_t bytes) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it == backends_.end()) return Status(StatusCode::NotFound, "backend not registered");
    it->second.capacity.committed_bytes += bytes;
    total_committed_ += bytes;
    return Status::ok_status();
}

Status ReservationLedger::remove_committed(StorageBackendId backend, std::uint64_t bytes) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it == backends_.end()) return Status(StatusCode::NotFound, "backend not registered");
    if (it->second.capacity.committed_bytes < bytes) {
        return Status(StatusCode::NegativeAccounting, "committed accounting underflow");
    }
    it->second.capacity.committed_bytes -= bytes;
    total_committed_ -= bytes;
    return Status::ok_status();
}

std::uint64_t ReservationLedger::reserved(StorageBackendId backend) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it == backends_.end()) return 0;
    return it->second.capacity.reserved_bytes;
}

std::uint64_t ReservationLedger::committed(StorageBackendId backend) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it == backends_.end()) return 0;
    return it->second.capacity.committed_bytes;
}

std::uint64_t ReservationLedger::free(StorageBackendId backend) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it == backends_.end()) return 0;
    const BackendCapacity& c = it->second.capacity;
    const std::uint64_t occ = c.reserved_bytes + c.committed_bytes;
    return c.free_bytes > occ ? c.free_bytes - occ : 0;
}

std::uint64_t ReservationLedger::reclaimable(StorageBackendId backend) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it == backends_.end()) return 0;
    return it->second.capacity.reclaimable_bytes;
}

Result<BackendCapacity> ReservationLedger::snapshot(StorageBackendId backend) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = backends_.find(backend);
    if (it == backends_.end()) {
        return Result<BackendCapacity>::failure(StatusCode::NotFound, "backend not registered");
    }
    return it->second.capacity;
}

// ---------------------------------------------------------------------------
// ReservationGuard
// ---------------------------------------------------------------------------
ReservationGuard::ReservationGuard(ReservationLedger* ledger, Reservation res, bool armed)
    : ledger_(ledger), res_(std::move(res)), armed_(armed) {}

ReservationGuard::~ReservationGuard() {
    if (ledger_ && armed_ && res_.state == ReservationState::ACTIVE) {
        ledger_->release(res_.id, res_.generation);
    }
}

ReservationGuard::ReservationGuard(ReservationGuard&& other) noexcept
    : ledger_(other.ledger_), res_(std::move(other.res_)), armed_(other.armed_) {
    other.disarm();
}

ReservationGuard& ReservationGuard::operator=(ReservationGuard&& other) noexcept {
    if (this != &other) {
        if (ledger_ && armed_ && res_.state == ReservationState::ACTIVE) {
            ledger_->release(res_.id, res_.generation);
        }
        ledger_ = other.ledger_;
        res_ = std::move(other.res_);
        armed_ = other.armed_;
        other.disarm();
    }
    return *this;
}

Status ReservationGuard::commit(PlacementId placement) {
    if (!ledger_) return Status(StatusCode::InvalidState, "no ledger bound");
    Status s = ledger_->commit(res_.id, placement);
    if (s.ok()) disarm();
    return s;
}

Status ReservationGuard::release() {
    if (!ledger_) return Status(StatusCode::InvalidState, "no ledger bound");
    Status s = ledger_->release(res_.id, res_.generation);
    if (s.ok()) disarm();
    return s;
}

}  // namespace storagefabric
