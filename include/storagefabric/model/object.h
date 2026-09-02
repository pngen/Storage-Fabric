#pragma once
// Storage Fabric - logical object descriptor.
// Object identity is independent of storage path. The same logical object may
// have many replicas and placements across tiers.

#include <cstdint>
#include <string>
#include <vector>
#include <map>

#include "storagefabric/core/strong.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/model/enums.h"
#include "storagefabric/model/authority.h"

namespace storagefabric {

// Durability requirement: minimum number of authoritative replicas that must
// survive. An object with durability 0 is treated as optional/ephemeral.
struct DurabilityRequirement {
    std::uint32_t min_replicas{1};

    bool is_satisfied_by(std::uint32_t replicas) const noexcept {
        return replicas >= min_replicas;
    }
};

// Locality preference: the storage class the owner prefers, if any.
struct LocalityPreference {
    StorageClass preferred{StorageClass::UNKNOWN};
    std::string node_hint;  // optional node hint; empty means any.
};

// A single provenance point for a logical object.
struct CreationProvenance {
    AuthorityOrigin origin{AuthorityOrigin::UNKNOWN};
    WorkerId worker;
    WorkerBootId boot;
    AuthorityGeneration authority_generation;
    std::string creator;  // free-form descriptor, e.g. "runner:producer"
};

// Compatibility metadata is a bounded set of key/value bytes.
using CompatibilityMetadata = std::map<std::string, std::string>;

struct ObjectDescriptor {
    ObjectId id;
    ObjectGeneration generation;
    ObjectKind kind{ObjectKind::GENERIC_AI_STATE};
    std::uint64_t logical_size{0};
    ContentDigest digest;               // digest of the canonical object bytes
    OwnerId owner;
    CreationProvenance provenance;
    DurabilityRequirement durability;
    RestorePriority restore_priority{RestorePriority::NORMAL};
    LocalityPreference locality;
    RetentionPolicy retention{RetentionPolicy::LRU_CLASS};
    PolicyGeneration policy_generation;
    std::vector<ObjectId> dependencies;
    CompatibilityMetadata compatibility;
    std::int64_t created_at_ms{0};

    bool has_valid_digest() const noexcept { return !digest.is_zero(); }
    bool is_well_formed() const noexcept {
        return !id.is_nil();
    }
    bool is_empty_kind() const noexcept { return kind == ObjectKind::GENERIC_AI_STATE; }
    void normalize() {
        if (durability.min_replicas == 0) durability.min_replicas = 1;
        if (restore_priority == RestorePriority::NORMAL &&
            (durability.min_replicas >= 2)) {
            // Durability beyond a single copy implies at least HIGH restore priority.
        }
    }
};

}  // namespace storagefabric
