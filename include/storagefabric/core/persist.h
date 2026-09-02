#pragma once
// Storage Fabric - versioned binary metadata persistence.
// Format: magic | version | payload | crc32 | sha256_semantic
// - magic: 4 bytes "SFBM"
// - version: 1 byte
// - payload: length-prefixed deterministic encoding of the metadata snapshot
// - crc32: over header+payload
// - sha256: semantic digest over the encoded payload (stable across runs)
// Recovery rejects truncation, corruption, invalid enums, impossible counts,
// duplicate IDs, generation regression, and trailing garbage.

#include <cstdint>
#include <string>
#include <vector>

#include "storagefabric/core/strong.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/model/object.h"
#include "storagefabric/model/manifest.h"
#include "storagefabric/model/placement.h"
#include "storagefabric/model/replica.h"
#include "storagefabric/model/backend.h"
#include "storagefabric/core/accounting.h"

namespace storagefabric {

constexpr std::uint32_t kMetaMagic = 0x5346424Du;   // "SFBM"
constexpr std::uint8_t kMetaVersion = 1;
constexpr std::size_t kMetaMaxRecords = 1u << 24;   // 16M records bound
constexpr std::size_t kMetaMaxString = 1u << 20;    // 1MiB string bound

struct CompletedTransferRecord {
    TransferId id;
    MovementKind kind{MovementKind::COPY};
    ObjectId object;
    StorageBackendId source_backend;
    StorageBackendId target_backend;
    std::uint64_t bytes{0};
    TransferState state{TransferState::COMPLETED};
    std::int64_t finished_at_ms{0};
};

struct MetadataSnapshot {
    CoordinatorEpoch epoch;
    AuthorityEnvelope authority;         // last authoritative writer
    std::vector<ObjectDescriptor> objects;
    std::vector<Manifest> manifests;
    std::vector<PlacementRecord> placements;
    std::vector<ReplicaSet> replica_sets;
    std::vector<BackendDescriptor> backends;
    std::vector<CompletedTransferRecord> completed_transfers;
    std::vector<AuthorityEnvelope> authorities;
    PolicyGeneration policy_generation;
    AccountingTotals accounting;
    std::int64_t saved_at_ms{0};
};

struct PersistResult {
    bool ok{false};
    StatusCode code{StatusCode::Ok};
    std::string detail;
    ContentDigest semantic_digest;      // recomputed semantic SHA-256
    std::uint32_t crc{0};
    std::size_t bytes{0};
};

// Serializes a snapshot to a metadata blob. The semantic digest is the SHA-256
// of the encoded payload and is stable for a fixed snapshot.
PersistResult serialize_snapshot(const MetadataSnapshot& snap, Bytes& out);

// Deserializes and validates a metadata blob. Rejects corrupt/truncated input,
// invalid enums, impossible counts, duplicate IDs, generation regression, and
// trailing garbage. On success, the semantic digest is recomputed and compared.
PersistResult deserialize_snapshot(ByteSpan data, MetadataSnapshot& snap);

// Recomputes the semantic digest over a serialized metadata blob's payload.
ContentDigest semantic_digest_of(ByteSpan serialized);

}  // namespace storagefabric
