#include "storagefabric/model/manifest.h"
#include "storagefabric/model/placement.h"
#include "storagefabric/model/replica.h"
#include "storagefabric/model/tier.h"

#include <algorithm>
#include <set>

namespace storagefabric {

const char* to_string(TierDurabilityClass v) noexcept {
    switch (v) {
        case TierDurabilityClass::EPHEMERAL: return "EPHEMERAL";
        case TierDurabilityClass::LOCAL: return "LOCAL";
        case TierDurabilityClass::REDUNDANT: return "REDUNDANT";
        case TierDurabilityClass::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
void Manifest::sort_chunks() {
    std::sort(chunks.begin(), chunks.end(), [](const ChunkDescriptor& a, const ChunkDescriptor& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.id.value() < b.id.value();
    });
}

Status Manifest::validate() const {
    const auto r = validate_manifest(*this, false);
    if (r.ok) return Status::ok_status();
    return Status(r.code, r.detail);
}

ManifestValidation validate_manifest(const Manifest& m, bool allow_gaps) {
    if (m.id.is_nil()) return {false, StatusCode::Malformed, "manifest id is nil"};
    if (m.object.is_nil()) return {false, StatusCode::Malformed, "manifest object id is nil"};
    if (m.total_logical_length == 0) return {false, StatusCode::Malformed, "zero-length manifest"};

    // Semantic digest must be set.
    if (m.manifest_digest.is_zero()) return {false, StatusCode::IntegrityMismatch, "manifest digest is zero"};

    // Chunks sorted deterministically by offset.
    Manifest copy = m;
    copy.sort_chunks();
    if (copy.chunks.size() != m.chunks.size()) {
        return {false, StatusCode::Malformed, "chunk count mismatch"};
    }
    for (size_t i = 0; i < m.chunks.size(); ++i) {
        if (copy.chunks[i].offset != m.chunks[i].offset ||
            copy.chunks[i].id != m.chunks[i].id) {
            return {false, StatusCode::Malformed, "manifest chunk ordering is not deterministic"};
        }
    }

    std::set<std::uint64_t> ids;
    std::uint64_t cursor = 0;
    for (const auto& c : m.chunks) {
        // Duplicate chunk id
        if (!ids.insert(c.id.value()).second) {
            return {false, StatusCode::DuplicateIdentity, "duplicate chunk id"};
        }
        if (c.id.is_nil() || c.blob.is_nil()) {
            return {false, StatusCode::Malformed, "chunk has nil id or blob"};
        }
        if (c.digest.is_zero()) return {false, StatusCode::IntegrityMismatch, "chunk digest is zero"};
        if (c.logical_length == 0) return {false, StatusCode::Malformed, "chunk has zero logical length"};
        if (c.physical_length == 0) return {false, StatusCode::Malformed, "chunk has zero physical length"};
        // Offset overflow guard
        if (c.offset > m.total_logical_length) {
            return {false, StatusCode::Overflow, "chunk offset exceeds object length"};
        }
        // Gap detection
        if (c.offset < cursor) {
            return {false, StatusCode::Malformed, "chunk overlap detected"};
        }
        if (!allow_gaps && c.offset != cursor) {
            return {false, StatusCode::Malformed, "chunk gap detected"};
        }
        // total-length mismatch / overflow
        if (c.offset + c.logical_length > m.total_logical_length) {
            return {false, StatusCode::LengthMismatch, "chunk extends beyond object length"};
        }
        cursor = c.offset + c.logical_length;
    }
    if (cursor != m.total_logical_length) {
        return {false, StatusCode::LengthMismatch, "manifest does not cover the full object length"};
    }
    if (m.chunks.empty()) {
        return {false, StatusCode::Malformed, "manifest has no chunks"};
    }
    return {true, StatusCode::Ok, ""};
}

// ---------------------------------------------------------------------------
// Placement lifecycle guards
// ---------------------------------------------------------------------------
bool PlacementRecord::is_valid_transition(PlacementLifecycle from, PlacementLifecycle to) noexcept {
    return can_transition(from, to);
}

bool can_transition(PlacementLifecycle from, PlacementLifecycle to) noexcept {
    // Guarded transitions: a placement only becomes AVAILABLE through the
    // verification/commit path. Terminal states (EVICTED, FAILED) may only be
    // re-entered through INVALIDATED (rebuild) for a fresh generation.
    switch (from) {
        case PlacementLifecycle::PLANNED:
            return to == PlacementLifecycle::RESERVED || to == PlacementLifecycle::FAILED ||
                   to == PlacementLifecycle::STALE || to == PlacementLifecycle::INVALIDATED;
        case PlacementLifecycle::RESERVED:
            return to == PlacementLifecycle::WRITING || to == PlacementLifecycle::FAILED ||
                   to == PlacementLifecycle::STALE || to == PlacementLifecycle::INVALIDATED;
        case PlacementLifecycle::WRITING:
            return to == PlacementLifecycle::VERIFYING || to == PlacementLifecycle::FAILED ||
                   to == PlacementLifecycle::STALE || to == PlacementLifecycle::INVALIDATED;
        case PlacementLifecycle::VERIFYING:
            return to == PlacementLifecycle::AVAILABLE || to == PlacementLifecycle::FAILED ||
                   to == PlacementLifecycle::STALE || to == PlacementLifecycle::INVALIDATED;
        case PlacementLifecycle::AVAILABLE:
            return to == PlacementLifecycle::DEGRADED || to == PlacementLifecycle::STALE ||
                   to == PlacementLifecycle::INVALIDATED || to == PlacementLifecycle::EVICTING;
        case PlacementLifecycle::DEGRADED:
            return to == PlacementLifecycle::AVAILABLE || to == PlacementLifecycle::STALE ||
                   to == PlacementLifecycle::INVALIDATED || to == PlacementLifecycle::EVICTING ||
                   to == PlacementLifecycle::FAILED;
        case PlacementLifecycle::STALE:
            return to == PlacementLifecycle::INVALIDATED || to == PlacementLifecycle::EVICTING ||
                   to == PlacementLifecycle::EVICTED;
        case PlacementLifecycle::INVALIDATED:
            return to == PlacementLifecycle::PLANNED || to == PlacementLifecycle::EVICTED;
        case PlacementLifecycle::EVICTING:
            return to == PlacementLifecycle::EVICTED || to == PlacementLifecycle::STALE ||
                   to == PlacementLifecycle::FAILED;
        case PlacementLifecycle::EVICTED:
            return false;                       // terminal
        case PlacementLifecycle::FAILED:
            return to == PlacementLifecycle::STALE || to == PlacementLifecycle::INVALIDATED;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Replica set state
// ---------------------------------------------------------------------------
ReplicaState ReplicaSet::compute_state() const {
    if (meets_durability() && distinct_failure_domains >= required) return ReplicaState::HEALTHY;
    if (authoritative_replicas == 0) return ReplicaState::FAILED;
    if (distinct_failure_domains == 0) return ReplicaState::STALE;
    return ReplicaState::UNDER_REPLICATED;
}

}  // namespace storagefabric
