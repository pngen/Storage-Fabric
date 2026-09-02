#pragma once
// Storage Fabric - model enumerations with explicit string conversion.
// Every enum has a stable text form and a bounded parser so that values
// round-trip through metadata and protocol deterministically.

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

namespace storagefabric {

// Governed machine-produced AI state kinds.
enum class ObjectKind : std::uint8_t {
    MODEL_SHARD,
    CHECKPOINT,
    TENSOR,
    KV_STATE,
    PREFIX_STATE,
    COMPILED_ARTIFACT,
    EXECUTION_GRAPH,
    EXECUTION_PLAN,
    ADAPTER,
    DATASET_SHARD,
    LOGICAL_BLOB,
    GENERIC_AI_STATE,
};

// Storage classes a tier may advertise.
enum class StorageClass : std::uint8_t {
    MEMORY_STAGING,
    LOCAL_NVME,
    LOCAL_FILESYSTEM,
    SHARED_FILESYSTEM,
    OBJECT_STORAGE_CLASS,
    REMOTE_BLOCK_CLASS,
    SYNTHETIC_REMOTE,
    UNKNOWN,
};

// How a numeric tier observation was obtained.
enum class MeasurementKind : std::uint8_t {
    MEASURED,
    REPORTED,
    DERIVED,
    SYNTHETIC,
    UNKNOWN,
};

// Freshness of observations and logical facts.
enum class Freshness : std::uint8_t {
    CURRENT,
    STALE,
    REVALIDATION_REQUIRED,
    UNKNOWN,
};

// Backend health state.
enum class Health : std::uint8_t {
    HEALTHY,
    DEGRADED,
    UNHEALTHY,
    UNAVAILABLE,
    UNKNOWN,
};

// Placement lifecycle.
enum class PlacementLifecycle : std::uint8_t {
    PLANNED,
    RESERVED,
    WRITING,
    VERIFYING,
    AVAILABLE,
    DEGRADED,
    STALE,
    INVALIDATED,
    EVICTING,
    EVICTED,
    FAILED,
};

// Replica set state.
enum class ReplicaState : std::uint8_t {
    UNDER_REPLICATED,
    HEALTHY,
    DEGRADED,
    REBUILDING,
    STALE,
    FAILED,
};

// Retention policy.
enum class RetentionPolicy : std::uint8_t {
    PINNED,
    PROTECTED,
    TTL,
    LRU_CLASS,
    RECOMPUTABLE,
    DURABILITY_REQUIRED,
};

// Restore priority.
enum class RestorePriority : std::uint8_t {
    CRITICAL,
    HIGH,
    NORMAL,
    LOW,
    BACKGROUND,
};

// Movement kinds.
enum class MovementKind : std::uint8_t {
    COPY,
    PROMOTE,
    DEMOTE,
    STAGE,
    RESTORE,
    EVICT,
    REPLICATE,
    REBUILD,
};

// Transfer state.
enum class TransferState : std::uint8_t {
    PLANNED,
    IN_FLIGHT,
    VERIFYING,
    COMPLETED,
    FAILED,
    CANCELLED,
    STALE,
};

// Reserved/committed capacity accounting state.
enum class ReservationState : std::uint8_t {
    ACTIVE,
    COMMITTED,
    RELEASED,
    EXPIRED,
};

// Plan constraint classification for named-factor reporting.
enum class PlanConstraintKind : std::uint8_t {
    HARD,
    SOFT,
};

// Source of a plan factor (what kind of evidence backs the number).
enum class PlanFactorOrigin : std::uint8_t {
    MEASURED,
    REPORTED,
    DERIVED,
    SYNTHETIC,
    UNKNOWN,
};

// Authority origin used in provenance records.
enum class AuthorityOrigin : std::uint8_t {
    USER,
    RUNNER,
    WORKER,
    COORDINATOR,
    RECOVERY,
    UNKNOWN,
};

// Backend capability flags.
enum class BackendCapability : std::uint8_t {
    kWrite,
    kRead,
    kDelete,
    kEnumerate,
    kFlush,
    kAtomicRename,
    kFreeSpaceQuery,
    kVerify,
    kDedupeCapable,
    kPersistent,
    kEvictable,
};

// --------------------------------- string conversion -------------------------

const char* to_string(ObjectKind) noexcept;
const char* to_string(StorageClass) noexcept;
const char* to_string(MeasurementKind) noexcept;
const char* to_string(Freshness) noexcept;
const char* to_string(Health) noexcept;
const char* to_string(PlacementLifecycle) noexcept;
const char* to_string(ReplicaState) noexcept;
const char* to_string(RetentionPolicy) noexcept;
const char* to_string(RestorePriority) noexcept;
const char* to_string(MovementKind) noexcept;
const char* to_string(TransferState) noexcept;
const char* to_string(ReservationState) noexcept;
const char* to_string(PlanConstraintKind) noexcept;
const char* to_string(PlanFactorOrigin) noexcept;
const char* to_string(AuthorityOrigin) noexcept;
const char* to_string(BackendCapability) noexcept;

// Bounded, case-insensitive parsers (return nullopt on unknown input).
std::optional<ObjectKind> parse_object_kind(std::string_view) noexcept;
std::optional<StorageClass> parse_storage_class(std::string_view) noexcept;
std::optional<MeasurementKind> parse_measurement_kind(std::string_view) noexcept;
std::optional<Freshness> parse_freshness(std::string_view) noexcept;
std::optional<Health> parse_health(std::string_view) noexcept;
std::optional<PlacementLifecycle> parse_placement_lifecycle(std::string_view) noexcept;
std::optional<ReplicaState> parse_replica_state(std::string_view) noexcept;
std::optional<RetentionPolicy> parse_retention_policy(std::string_view) noexcept;
std::optional<RestorePriority> parse_restore_priority(std::string_view) noexcept;
std::optional<MovementKind> parse_movement_kind(std::string_view) noexcept;
std::optional<TransferState> parse_transfer_state(std::string_view) noexcept;

}  // namespace storagefabric
