#pragma once

// Storage Fabric - strong identity and generation types.
// These types are non-interchangeable: an ObjectId may not be used where
// a BlobId is required, and a PlacementId never compares equal to a ReplicaId.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <ostream>

namespace storagefabric {

// Tag types. Each names a distinct identity or generation stamp so that the
// strongly typed wrapper below is non-interchangeable across domains.
struct ObjectIdTag {};
struct ObjectVersionIdTag {};
struct BlobIdTag {};
struct ChunkIdTag {};
struct ManifestIdTag {};
struct ReplicaIdTag {};
struct PlacementIdTag {};
struct StorageTierIdTag {};
struct StorageBackendIdTag {};
struct StorageNodeIdTag {};
struct VolumeIdTag {};
struct PathIdTag {};
struct OwnerIdTag {};
struct WorkerIdTag {};
struct WorkerBootIdTag {};
struct ReservationIdTag {};
struct TransferIdTag {};
struct RestoreIdTag {};
struct AttemptIdTag {};
struct DispatchIdTag {};
struct ObservationIdTag {};
struct PolicyIdTag {};

struct CoordinatorEpochTag {};
struct ObjectGenerationTag {};
struct ReplicaGenerationTag {};
struct PlacementGenerationTag {};
struct BackendGenerationTag {};
struct VolumeGenerationTag {};
struct ManifestGenerationTag {};
struct ChunkGenerationTag {};
struct AuthorityGenerationTag {};
struct ReservationGenerationTag {};
struct TransferGenerationTag {};
struct RestoreGenerationTag {};
struct AttemptGenerationTag {};
struct DispatchGenerationTag {};
struct ObservationGenerationTag {};
struct PolicyGenerationTag {};
struct HealthGenerationTag {};

// A strongly typed, 64-bit identity handle. Value 0 is the nil / unset value.
template <typename Tag>
class StrongId {
public:
    using tag_type = Tag;
    using value_type = std::uint64_t;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(value_type v) noexcept : value_(v) {}
    explicit constexpr StrongId(int v) noexcept : value_(static_cast<value_type>(v)) {}

    constexpr value_type value() const noexcept { return value_; }
    constexpr bool is_nil() const noexcept { return value_ == 0; }
    constexpr explicit operator bool() const noexcept { return value_ != 0; }

    constexpr StrongId next() const noexcept { return StrongId(value_ + 1); }

    constexpr bool operator==(const StrongId& o) const noexcept = default;
    constexpr auto operator<=>(const StrongId& o) const noexcept = default;

    std::string str() const { return std::to_string(value_); }
    friend std::ostream& operator<<(std::ostream& os, const StrongId& id) {
        return os << id.str();
    }

private:
    value_type value_{0};
};

// A strongly typed, 64-bit generation stamp. Generations only advance; a
// generation never "wraps" in normal operation. Generation comparisons are
// explicit and are only meaningful within a single authority context (same
// coordinator epoch and worker boot). Cross-authority freshness is decided by
// the authority envelope, not by a bare generation number.
template <typename Tag>
class Generation {
public:
    using tag_type = Tag;
    using value_type = std::uint64_t;

    constexpr Generation() noexcept = default;
    explicit constexpr Generation(value_type v) noexcept : value_(v) {}
    explicit constexpr Generation(int v) noexcept : value_(static_cast<value_type>(v)) {}

    constexpr value_type value() const noexcept { return value_; }
    constexpr bool is_nil() const noexcept { return value_ == 0; }
    constexpr explicit operator bool() const noexcept { return value_ != 0; }

    // Returns the next generation after this one (this + 1).
    constexpr Generation next() const noexcept { return Generation(value_ + 1); }

    // Explicit ordering within an authority context. The wrapper operators are
    // retained for ergonomics; equivalent textual forms are documented.
    constexpr bool exceeds(const Generation& o) const noexcept { return value_ > o.value_; }
    constexpr bool precedes(const Generation& o) const noexcept { return value_ < o.value_; }
    constexpr bool is_fresh_after(const Generation& o) const noexcept { return value_ > o.value_; }

    // True when the two generations denote the same stamp.
    constexpr bool coincident_with(const Generation& o) const noexcept { return value_ == o.value_; }

    constexpr bool operator==(const Generation& o) const noexcept = default;
    constexpr auto operator<=>(const Generation& o) const noexcept = default;

    std::string str() const { return std::to_string(value_); }
    friend std::ostream& operator<<(std::ostream& os, const Generation& g) {
        return os << g.str();
    }

private:
    value_type value_{0};
};

// Concrete identity aliases.
using ObjectId = StrongId<ObjectIdTag>;
using ObjectVersionId = StrongId<ObjectVersionIdTag>;
using BlobId = StrongId<BlobIdTag>;
using ChunkId = StrongId<ChunkIdTag>;
using ManifestId = StrongId<ManifestIdTag>;
using ReplicaId = StrongId<ReplicaIdTag>;
using PlacementId = StrongId<PlacementIdTag>;
using StorageTierId = StrongId<StorageTierIdTag>;
using StorageBackendId = StrongId<StorageBackendIdTag>;
using StorageNodeId = StrongId<StorageNodeIdTag>;
using VolumeId = StrongId<VolumeIdTag>;
using PathId = StrongId<PathIdTag>;
using OwnerId = StrongId<OwnerIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using ReservationId = StrongId<ReservationIdTag>;
using TransferId = StrongId<TransferIdTag>;
using RestoreId = StrongId<RestoreIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using DispatchId = StrongId<DispatchIdTag>;
using ObservationId = StrongId<ObservationIdTag>;
using PolicyId = StrongId<PolicyIdTag>;

// Concrete generation aliases.
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using ObjectGeneration = Generation<ObjectGenerationTag>;
using ReplicaGeneration = Generation<ReplicaGenerationTag>;
using PlacementGeneration = Generation<PlacementGenerationTag>;
using BackendGeneration = Generation<BackendGenerationTag>;
using VolumeGeneration = Generation<VolumeGenerationTag>;
using ManifestGeneration = Generation<ManifestGenerationTag>;
using ChunkGeneration = Generation<ChunkGenerationTag>;
using AuthorityGeneration = Generation<AuthorityGenerationTag>;
using ReservationGeneration = Generation<ReservationGenerationTag>;
using TransferGeneration = Generation<TransferGenerationTag>;
using RestoreGeneration = Generation<RestoreGenerationTag>;
using AttemptGeneration = Generation<AttemptGenerationTag>;
using DispatchGeneration = Generation<DispatchGenerationTag>;
using ObservationGeneration = Generation<ObservationGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using HealthGeneration = Generation<HealthGenerationTag>;

// The monotonic coordinator epoch. A newly started coordinator always begins
// with a fresh epoch strictly greater than any previously observed epoch.
struct CoordinatorEpochInfo {
    CoordinatorEpoch epoch;
    explicit CoordinatorEpochInfo(CoordinatorEpoch e = CoordinatorEpoch()) noexcept : epoch(e) {}
};

}  // namespace storagefabric

// std::hash specializations so the strong types may be used as map keys.
namespace std {

#define STORAGEFABRIC_DEFINE_STRONG_HASH(TYPE)                                   \
    template <>                                                                 \
    struct hash<::storagefabric::TYPE> {                                        \
        size_t operator()(const ::storagefabric::TYPE& v) const noexcept {      \
            return hash<typename ::storagefabric::TYPE::value_type>()(v.value());\
        }                                                                       \
    };

STORAGEFABRIC_DEFINE_STRONG_HASH(ObjectId)
STORAGEFABRIC_DEFINE_STRONG_HASH(ObjectVersionId)
STORAGEFABRIC_DEFINE_STRONG_HASH(BlobId)
STORAGEFABRIC_DEFINE_STRONG_HASH(ChunkId)
STORAGEFABRIC_DEFINE_STRONG_HASH(ManifestId)
STORAGEFABRIC_DEFINE_STRONG_HASH(ReplicaId)
STORAGEFABRIC_DEFINE_STRONG_HASH(PlacementId)
STORAGEFABRIC_DEFINE_STRONG_HASH(StorageTierId)
STORAGEFABRIC_DEFINE_STRONG_HASH(StorageBackendId)
STORAGEFABRIC_DEFINE_STRONG_HASH(StorageNodeId)
STORAGEFABRIC_DEFINE_STRONG_HASH(VolumeId)
STORAGEFABRIC_DEFINE_STRONG_HASH(PathId)
STORAGEFABRIC_DEFINE_STRONG_HASH(OwnerId)
STORAGEFABRIC_DEFINE_STRONG_HASH(WorkerId)
STORAGEFABRIC_DEFINE_STRONG_HASH(WorkerBootId)
STORAGEFABRIC_DEFINE_STRONG_HASH(ReservationId)
STORAGEFABRIC_DEFINE_STRONG_HASH(TransferId)
STORAGEFABRIC_DEFINE_STRONG_HASH(RestoreId)
STORAGEFABRIC_DEFINE_STRONG_HASH(AttemptId)
STORAGEFABRIC_DEFINE_STRONG_HASH(DispatchId)
STORAGEFABRIC_DEFINE_STRONG_HASH(ObservationId)
STORAGEFABRIC_DEFINE_STRONG_HASH(PolicyId)
STORAGEFABRIC_DEFINE_STRONG_HASH(CoordinatorEpoch)
STORAGEFABRIC_DEFINE_STRONG_HASH(ObjectGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(ReplicaGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(PlacementGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(BackendGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(VolumeGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(ManifestGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(ChunkGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(AuthorityGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(ReservationGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(TransferGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(RestoreGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(AttemptGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(DispatchGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(ObservationGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(PolicyGeneration)
STORAGEFABRIC_DEFINE_STRONG_HASH(HealthGeneration)

}  // namespace std
