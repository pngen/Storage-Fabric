#pragma once
// Storage Fabric - top-level governed storage runtime.
// Owns the tier/backend registry, the object/replica/placement catalog, the
// reservation ledger, authority fencing, generation-aware publication, and the
// persistence recovery path. It exposes the explanation API and accounting.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <optional>

#include "storagefabric/core/strong.h"
#include "storagefabric/core/status.h"
#include "storagefabric/core/planner.h"
#include "storagefabric/core/capacity.h"
#include "storagefabric/core/accounting.h"
#include "storagefabric/core/persist.h"
#include "storagefabric/model/object.h"
#include "storagefabric/model/manifest.h"
#include "storagefabric/model/placement.h"
#include "storagefabric/model/replica.h"
#include "storagefabric/model/backend.h"
#include "storagefabric/model/authority.h"
#include "storagefabric/storage/backend.h"

namespace storagefabric {

struct SyntheticProfile;  // defined in storage/synthetic_backend.h

struct PublishOptions {
    PolicyGeneration policy_generation;
    AuthorityEnvelope authority;          // optional; otherwise uses runtime authority
    std::uint32_t required_replicas{1};
    bool dedupe{true};
    bool eager_verify{true};
    std::uint64_t chunk_size{0};         // 0 = single chunk
};

struct EvictionDecision {
    bool allowed{false};
    std::string reason;
    StatusCode code{StatusCode::Ok};
};

class StorageFabric {
public:
    StorageFabric() = default;

    // ---- authority ----
    void set_authority(AuthorityEnvelope a) { authority_ = std::move(a); }
    const AuthorityEnvelope& authority() const noexcept { return authority_; }
    void new_coordinator_epoch() { authority_.epoch = authority_.epoch.next(); }

    // ---- tier + backend registration ----
    Result<StorageBackendId> register_local_backend(const std::string& name,
                                                    const std::filesystem::path& root,
                                                    StorageClass storage_class = StorageClass::LOCAL_FILESYSTEM);
    Result<StorageBackendId> register_synthetic_backend(const std::string& name,
                                                        const SyntheticProfile& profile);

    // ---- object definition ----
    Result<ObjectDescriptor> define_object(ObjectKind kind, std::uint64_t logical_size,
                                           ByteSpan content, const AuthorityEnvelope& writer,
                                           const DurabilityRequirement& durability,
                                           RestorePriority restore_priority = RestorePriority::NORMAL);

    // ---- planning ----
    StoragePlan plan(const PlanRequest& req) const;
    PlanRequest make_plan_request(const ObjectDescriptor& obj,
                                    std::uint32_t required_replicas) const;

    // ---- transactional publish ----
    Result<PlacementRecord> publish(const ObjectDescriptor& obj, ByteSpan content,
                                    const PublishOptions& opts);
    Result<PlacementRecord> publish_to(const ObjectDescriptor& obj, ByteSpan content,
                                       StorageBackendId backend, const PublishOptions& opts);

    // ---- read / verify ----
    Result<Bytes> read(ObjectId object, std::optional<PlacementId> placement = std::nullopt) const;
    Result<VerifyResult> verify(PlacementId placement) const;
    Result<PlacementRecord> find_authoritative_placement(ObjectId object) const;

    // ---- replication / movement ----
    Result<PlacementRecord> replicate(const PlacementRecord& source, StorageBackendId target,
                                      const PublishOptions& opts);
    Result<PlacementRecord> move(const PlacementRecord& source, StorageBackendId target,
                                 const PublishOptions& opts);

    // ---- eviction / release ----
    EvictionDecision can_evict(PlacementId placement) const;
    Result<EvictionDecision> evict(PlacementId placement);

    // ---- persistence ----
    Status save(const std::filesystem::path& path);
    Status recover(const std::filesystem::path& path);

    // ---- explanation API ----
    std::string explain_placement(ObjectId object) const;
    std::string explain_read_source(PlacementId placement) const;
    std::string explain_replication(ObjectId object) const;
    std::string explain_eviction(PlacementId placement) const;
    std::string explain_restore(ObjectId object) const;
    std::string explain_failure(ObjectId object) const;
    std::string explain_recovery() const;
    std::string explain_backend(StorageBackendId backend) const;

    // ---- observability ----
    AccountingTotals accounting() const { return accounting_; }
    std::vector<ObjectDescriptor> objects() const;
    std::vector<PlacementRecord> placements() const;
    std::vector<ReplicaSet> replica_sets() const;
    std::size_t object_count() const { return catalog_.size(); }
    std::size_t placement_count() const { return placements_.size(); }

    StorageBackend* backend(StorageBackendId id) const;
    const StorageTier* tier(StorageTierId id) const;
    const BackendDescriptor* backend_descriptor(StorageBackendId id) const;

private:
    struct CatalogEntry {
        ObjectDescriptor object;
        Manifest manifest;
        ReplicaSet replica_set;
        std::vector<PlacementId> placements;
        std::vector<PlacementId> stale_placements;
    };

    Result<std::shared_ptr<StorageBackend>> get_backend(StorageBackendId id) const;
    CatalogEntry* get_entry(ObjectId object);
    const CatalogEntry* get_entry(ObjectId object) const;

    StorageBackendId alloc_backend_id();
    StorageTierId alloc_tier_id();
    ObjectId alloc_object_id();
    ManifestId alloc_manifest_id();
    ReplicaId alloc_replica_id();
    PlacementId alloc_placement_id();

    Result<Reservation> reserve_for(const ObjectDescriptor& obj, StorageBackendId backend,
                                    const PublishOptions& opts);

    mutable std::mutex mutex_;
    AuthorityEnvelope authority_;
    PolicyGeneration policy_generation_;

    std::unordered_map<StorageBackendId, std::shared_ptr<StorageBackend>> backends_;
    std::unordered_map<StorageTierId, StorageTier> tiers_;
    std::unordered_map<StorageBackendId, BackendDescriptor> backend_descriptors_;
    std::unordered_map<ObjectId, CatalogEntry> catalog_;
    std::unordered_map<PlacementId, PlacementRecord> placements_;
    std::unordered_map<ManifestId, Manifest> manifests_;
    std::unordered_map<StorageBackendId, BackendCapacity> capacity_probe_;

    ReservationLedger reservations_;
    mutable AccountingTotals accounting_;

    // Content-addressed blob store (dedup + refcount).
    std::unordered_map<ContentDigest, BlobId> blob_by_digest_;
    std::unordered_map<BlobId, ContentDigest> blob_digest_;
    std::unordered_map<BlobId, std::uint64_t> blob_refs_;
    std::unordered_map<BlobId, std::uint64_t> blob_sizes_;

    // Id counters (monotonic, restored from persistence).
    std::uint64_t backend_counter_{1};
    std::uint64_t tier_counter_{1};
    std::uint64_t object_counter_{1};
    std::uint64_t manifest_counter_{1};
    std::uint64_t replica_counter_{1};
    std::uint64_t placement_counter_{1};
    std::uint64_t blob_counter_{1};

    bool recovered_{false};
    std::string recovery_note_;
    std::filesystem::path last_save_path_;
};

}  // namespace storagefabric
