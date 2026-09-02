#include "storagefabric/model/enums.h"

#include <cstring>
#include <algorithm>
#include <array>

namespace storagefabric {

namespace {
// Case-insensitive ASCII compare.
bool iequals(std::string_view a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        char cb = b[i];
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

template <typename E, typename K, size_t N>
std::optional<E> parse(const std::array<std::pair<K, const char*>, N>& table, std::string_view s) {
    for (const auto& entry : table) {
        if (iequals(s, entry.second)) return static_cast<E>(entry.first);
    }
    return std::nullopt;
}
}  // namespace

#define SFB_TOSTRING(ENUM, CASES)                \
    switch (ENUM) {                              \
        CASES                                    \
        default: return "Unknown";               \
    }

#define SFB_CASE(E, S) case E: return S;

const char* to_string(ObjectKind v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(ObjectKind::MODEL_SHARD, "MODEL_SHARD")
        SFB_CASE(ObjectKind::CHECKPOINT, "CHECKPOINT")
        SFB_CASE(ObjectKind::TENSOR, "TENSOR")
        SFB_CASE(ObjectKind::KV_STATE, "KV_STATE")
        SFB_CASE(ObjectKind::PREFIX_STATE, "PREFIX_STATE")
        SFB_CASE(ObjectKind::COMPILED_ARTIFACT, "COMPILED_ARTIFACT")
        SFB_CASE(ObjectKind::EXECUTION_GRAPH, "EXECUTION_GRAPH")
        SFB_CASE(ObjectKind::EXECUTION_PLAN, "EXECUTION_PLAN")
        SFB_CASE(ObjectKind::ADAPTER, "ADAPTER")
        SFB_CASE(ObjectKind::DATASET_SHARD, "DATASET_SHARD")
        SFB_CASE(ObjectKind::LOGICAL_BLOB, "LOGICAL_BLOB")
        SFB_CASE(ObjectKind::GENERIC_AI_STATE, "GENERIC_AI_STATE")
    )
}

const char* to_string(StorageClass v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(StorageClass::MEMORY_STAGING, "MEMORY_STAGING")
        SFB_CASE(StorageClass::LOCAL_NVME, "LOCAL_NVME")
        SFB_CASE(StorageClass::LOCAL_FILESYSTEM, "LOCAL_FILESYSTEM")
        SFB_CASE(StorageClass::SHARED_FILESYSTEM, "SHARED_FILESYSTEM")
        SFB_CASE(StorageClass::OBJECT_STORAGE_CLASS, "OBJECT_STORAGE_CLASS")
        SFB_CASE(StorageClass::REMOTE_BLOCK_CLASS, "REMOTE_BLOCK_CLASS")
        SFB_CASE(StorageClass::SYNTHETIC_REMOTE, "SYNTHETIC_REMOTE")
        SFB_CASE(StorageClass::UNKNOWN, "UNKNOWN")
    )
}

const char* to_string(MeasurementKind v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(MeasurementKind::MEASURED, "MEASURED")
        SFB_CASE(MeasurementKind::REPORTED, "REPORTED")
        SFB_CASE(MeasurementKind::DERIVED, "DERIVED")
        SFB_CASE(MeasurementKind::SYNTHETIC, "SYNTHETIC")
        SFB_CASE(MeasurementKind::UNKNOWN, "UNKNOWN")
    )
}

const char* to_string(Freshness v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(Freshness::CURRENT, "CURRENT")
        SFB_CASE(Freshness::STALE, "STALE")
        SFB_CASE(Freshness::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED")
        SFB_CASE(Freshness::UNKNOWN, "UNKNOWN")
    )
}

const char* to_string(Health v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(Health::HEALTHY, "HEALTHY")
        SFB_CASE(Health::DEGRADED, "DEGRADED")
        SFB_CASE(Health::UNHEALTHY, "UNHEALTHY")
        SFB_CASE(Health::UNAVAILABLE, "UNAVAILABLE")
        SFB_CASE(Health::UNKNOWN, "UNKNOWN")
    )
}

const char* to_string(PlacementLifecycle v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(PlacementLifecycle::PLANNED, "PLANNED")
        SFB_CASE(PlacementLifecycle::RESERVED, "RESERVED")
        SFB_CASE(PlacementLifecycle::WRITING, "WRITING")
        SFB_CASE(PlacementLifecycle::VERIFYING, "VERIFYING")
        SFB_CASE(PlacementLifecycle::AVAILABLE, "AVAILABLE")
        SFB_CASE(PlacementLifecycle::DEGRADED, "DEGRADED")
        SFB_CASE(PlacementLifecycle::STALE, "STALE")
        SFB_CASE(PlacementLifecycle::INVALIDATED, "INVALIDATED")
        SFB_CASE(PlacementLifecycle::EVICTING, "EVICTING")
        SFB_CASE(PlacementLifecycle::EVICTED, "EVICTED")
        SFB_CASE(PlacementLifecycle::FAILED, "FAILED")
    )
}

const char* to_string(ReplicaState v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(ReplicaState::UNDER_REPLICATED, "UNDER_REPLICATED")
        SFB_CASE(ReplicaState::HEALTHY, "HEALTHY")
        SFB_CASE(ReplicaState::DEGRADED, "DEGRADED")
        SFB_CASE(ReplicaState::REBUILDING, "REBUILDING")
        SFB_CASE(ReplicaState::STALE, "STALE")
        SFB_CASE(ReplicaState::FAILED, "FAILED")
    )
}

const char* to_string(RetentionPolicy v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(RetentionPolicy::PINNED, "PINNED")
        SFB_CASE(RetentionPolicy::PROTECTED, "PROTECTED")
        SFB_CASE(RetentionPolicy::TTL, "TTL")
        SFB_CASE(RetentionPolicy::LRU_CLASS, "LRU_CLASS")
        SFB_CASE(RetentionPolicy::RECOMPUTABLE, "RECOMPUTABLE")
        SFB_CASE(RetentionPolicy::DURABILITY_REQUIRED, "DURABILITY_REQUIRED")
    )
}

const char* to_string(RestorePriority v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(RestorePriority::CRITICAL, "CRITICAL")
        SFB_CASE(RestorePriority::HIGH, "HIGH")
        SFB_CASE(RestorePriority::NORMAL, "NORMAL")
        SFB_CASE(RestorePriority::LOW, "LOW")
        SFB_CASE(RestorePriority::BACKGROUND, "BACKGROUND")
    )
}

const char* to_string(MovementKind v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(MovementKind::COPY, "COPY")
        SFB_CASE(MovementKind::PROMOTE, "PROMOTE")
        SFB_CASE(MovementKind::DEMOTE, "DEMOTE")
        SFB_CASE(MovementKind::STAGE, "STAGE")
        SFB_CASE(MovementKind::RESTORE, "RESTORE")
        SFB_CASE(MovementKind::EVICT, "EVICT")
        SFB_CASE(MovementKind::REPLICATE, "REPLICATE")
        SFB_CASE(MovementKind::REBUILD, "REBUILD")
    )
}

const char* to_string(TransferState v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(TransferState::PLANNED, "PLANNED")
        SFB_CASE(TransferState::IN_FLIGHT, "IN_FLIGHT")
        SFB_CASE(TransferState::VERIFYING, "VERIFYING")
        SFB_CASE(TransferState::COMPLETED, "COMPLETED")
        SFB_CASE(TransferState::FAILED, "FAILED")
        SFB_CASE(TransferState::CANCELLED, "CANCELLED")
        SFB_CASE(TransferState::STALE, "STALE")
    )
}

const char* to_string(ReservationState v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(ReservationState::ACTIVE, "ACTIVE")
        SFB_CASE(ReservationState::COMMITTED, "COMMITTED")
        SFB_CASE(ReservationState::RELEASED, "RELEASED")
        SFB_CASE(ReservationState::EXPIRED, "EXPIRED")
    )
}

const char* to_string(PlanConstraintKind v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(PlanConstraintKind::HARD, "HARD")
        SFB_CASE(PlanConstraintKind::SOFT, "SOFT")
    )
}

const char* to_string(PlanFactorOrigin v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(PlanFactorOrigin::MEASURED, "MEASURED")
        SFB_CASE(PlanFactorOrigin::REPORTED, "REPORTED")
        SFB_CASE(PlanFactorOrigin::DERIVED, "DERIVED")
        SFB_CASE(PlanFactorOrigin::SYNTHETIC, "SYNTHETIC")
        SFB_CASE(PlanFactorOrigin::UNKNOWN, "UNKNOWN")
    )
}

const char* to_string(AuthorityOrigin v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(AuthorityOrigin::USER, "USER")
        SFB_CASE(AuthorityOrigin::RUNNER, "RUNNER")
        SFB_CASE(AuthorityOrigin::WORKER, "WORKER")
        SFB_CASE(AuthorityOrigin::COORDINATOR, "COORDINATOR")
        SFB_CASE(AuthorityOrigin::RECOVERY, "RECOVERY")
        SFB_CASE(AuthorityOrigin::UNKNOWN, "UNKNOWN")
    )
}

const char* to_string(BackendCapability v) noexcept {
    SFB_TOSTRING(v,
        SFB_CASE(BackendCapability::kWrite, "WRITE")
        SFB_CASE(BackendCapability::kRead, "READ")
        SFB_CASE(BackendCapability::kDelete, "DELETE")
        SFB_CASE(BackendCapability::kEnumerate, "ENUMERATE")
        SFB_CASE(BackendCapability::kFlush, "FLUSH")
        SFB_CASE(BackendCapability::kAtomicRename, "ATOMIC_RENAME")
        SFB_CASE(BackendCapability::kFreeSpaceQuery, "FREE_SPACE_QUERY")
        SFB_CASE(BackendCapability::kVerify, "VERIFY")
        SFB_CASE(BackendCapability::kDedupeCapable, "DEDUPE_CAPABLE")
        SFB_CASE(BackendCapability::kPersistent, "PERSISTENT")
        SFB_CASE(BackendCapability::kEvictable, "EVICTABLE")
    )
}

// ---------------------------------------------------------------------------
// Parsers. The order of entries does not matter; comparison is case-insensitive.
// ---------------------------------------------------------------------------

std::optional<ObjectKind> parse_object_kind(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 12> table{{
        {0, "MODEL_SHARD"}, {1, "CHECKPOINT"}, {2, "TENSOR"}, {3, "KV_STATE"},
        {4, "PREFIX_STATE"}, {5, "COMPILED_ARTIFACT"}, {6, "EXECUTION_GRAPH"},
        {7, "EXECUTION_PLAN"}, {8, "ADAPTER"}, {9, "DATASET_SHARD"},
        {10, "LOGICAL_BLOB"}, {11, "GENERIC_AI_STATE"}}};
    auto r = parse<ObjectKind>(table, s);
    return r;
}

std::optional<StorageClass> parse_storage_class(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 8> table{{
        {0, "MEMORY_STAGING"}, {1, "LOCAL_NVME"}, {2, "LOCAL_FILESYSTEM"},
        {3, "SHARED_FILESYSTEM"}, {4, "OBJECT_STORAGE_CLASS"},
        {5, "REMOTE_BLOCK_CLASS"}, {6, "SYNTHETIC_REMOTE"}, {7, "UNKNOWN"}}};
    auto r = parse<StorageClass>(table, s);
    return r;
}

std::optional<MeasurementKind> parse_measurement_kind(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 5> table{{
        {0, "MEASURED"}, {1, "REPORTED"}, {2, "DERIVED"}, {3, "SYNTHETIC"}, {4, "UNKNOWN"}}};
    auto r = parse<MeasurementKind>(table, s);
    return r;
}

std::optional<Freshness> parse_freshness(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 4> table{{
        {0, "CURRENT"}, {1, "STALE"}, {2, "REVALIDATION_REQUIRED"}, {3, "UNKNOWN"}}};
    auto r = parse<Freshness>(table, s);
    return r;
}

std::optional<Health> parse_health(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 5> table{{
        {0, "HEALTHY"}, {1, "DEGRADED"}, {2, "UNHEALTHY"}, {3, "UNAVAILABLE"}, {4, "UNKNOWN"}}};
    auto r = parse<Health>(table, s);
    return r;
}

std::optional<PlacementLifecycle> parse_placement_lifecycle(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 11> table{{
        {0, "PLANNED"}, {1, "RESERVED"}, {2, "WRITING"}, {3, "VERIFYING"},
        {4, "AVAILABLE"}, {5, "DEGRADED"}, {6, "STALE"}, {7, "INVALIDATED"},
        {8, "EVICTING"}, {9, "EVICTED"}, {10, "FAILED"}}};
    auto r = parse<PlacementLifecycle>(table, s);
    return r;
}

std::optional<ReplicaState> parse_replica_state(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 6> table{{
        {0, "UNDER_REPLICATED"}, {1, "HEALTHY"}, {2, "DEGRADED"}, {3, "REBUILDING"},
        {4, "STALE"}, {5, "FAILED"}}};
    auto r = parse<ReplicaState>(table, s);
    return r;
}

std::optional<RetentionPolicy> parse_retention_policy(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 6> table{{
        {0, "PINNED"}, {1, "PROTECTED"}, {2, "TTL"}, {3, "LRU_CLASS"},
        {4, "RECOMPUTABLE"}, {5, "DURABILITY_REQUIRED"}}};
    auto r = parse<RetentionPolicy>(table, s);
    return r;
}

std::optional<RestorePriority> parse_restore_priority(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 5> table{{
        {0, "CRITICAL"}, {1, "HIGH"}, {2, "NORMAL"}, {3, "LOW"}, {4, "BACKGROUND"}}};
    auto r = parse<RestorePriority>(table, s);
    return r;
}

std::optional<MovementKind> parse_movement_kind(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 8> table{{
        {0, "COPY"}, {1, "PROMOTE"}, {2, "DEMOTE"}, {3, "STAGE"}, {4, "RESTORE"},
        {5, "EVICT"}, {6, "REPLICATE"}, {7, "REBUILD"}}};
    auto r = parse<MovementKind>(table, s);
    return r;
}

std::optional<TransferState> parse_transfer_state(std::string_view s) noexcept {
    static const std::array<std::pair<int, const char*>, 7> table{{
        {0, "PLANNED"}, {1, "IN_FLIGHT"}, {2, "VERIFYING"}, {3, "COMPLETED"},
        {4, "FAILED"}, {5, "CANCELLED"}, {6, "STALE"}}};
    auto r = parse<TransferState>(table, s);
    return r;
}

#undef SFB_TOSTRING
#undef SFB_CASE

}  // namespace storagefabric
