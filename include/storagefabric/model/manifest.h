#pragma once
// Storage Fabric - chunk and manifest model with deterministic validation.
// A manifest deterministically orders chunks by offset. Validation rejects
// gaps, overlaps, duplicate chunks, non-covering manifests, offset overflow,
// total-length mismatch, and stale chunk generation.

#include <cstdint>
#include <vector>
#include <string>

#include "storagefabric/core/strong.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/model/enums.h"

namespace storagefabric {

// A single chunk of a logical object, addressed content-additively via a blob.
struct ChunkDescriptor {
    ChunkId id;
    ChunkGeneration generation;
    std::uint64_t offset{0};
    std::uint64_t logical_length{0};   // length in the logical object
    std::uint64_t physical_length{0};  // stored length (may differ if compressed)
    ContentDigest digest;              // digest of the stored physical bytes
    BlobId blob;                       // physical blob this chunk maps to
    AuthorityOrigin provenance{AuthorityOrigin::UNKNOWN};

    bool ends_at(std::uint64_t total) const noexcept {
        return offset <= total && (offset + logical_length) <= total;
    }
    bool is_valid(std::uint64_t total) const noexcept {
        return !id.is_nil() && logical_length > 0 && physical_length > 0 &&
               !digest.is_zero() && !blob.is_nil() && ends_at(total);
    }
};

struct Manifest {
    ManifestId id;
    ManifestGeneration generation;
    ObjectId object;
    ObjectGeneration object_generation;
    std::uint64_t total_logical_length{0};
    ContentDigest manifest_digest;   // semantic digest over the manifest record
    std::vector<ChunkDescriptor> chunks;

    // Chunks are kept sorted by offset for determinism.
    void sort_chunks();

    // Validates structural invariants. On success returns Ok; otherwise the
    // Status explains the violation. extra bytes beyond total are rejected.
    Status validate() const;
};

// Result of validating a manifest; carries a human-readable outcome.
struct ManifestValidation {
    bool ok{false};
    StatusCode code{StatusCode::Ok};
    std::string detail;
};

ManifestValidation validate_manifest(const Manifest& m, bool allow_gaps = false);

}  // namespace storagefabric
