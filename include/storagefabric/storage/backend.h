#pragma once
// Storage Fabric - storage backend contract.
// A backend owns the physical-copy/IO side of a placement. Storage Fabric never
// treats a path as an identity; it routes governed keys through a backend.
// Backends must be path-safe: keys are validated, never absolute, never .. and
// never escape the backend root.

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "storagefabric/core/strong.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/status.h"
#include "storagefabric/model/backend.h"
#include "storagefabric/model/tier.h"

namespace storagefabric {

// Result of an integrity verification.
struct VerifyResult {
    bool ok{false};
    StatusCode code{StatusCode::Ok};
    std::size_t size{0};
    ContentDigest digest;   // digest recomputed from stored bytes
};

// Validates a governed backend-relative key. Rejects absolute paths, parent
// traversal, embedded NUL, reserved names, and empty/blank keys.
Status validate_governed_key(std::string_view key) noexcept;

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual StorageBackendId id() const noexcept = 0;
    virtual const BackendDescriptor& descriptor() const noexcept = 0;
    virtual const StorageTier& tier() const noexcept = 0;

    // Writes bytes to the backend under a governed key, flushing according to
    // the backend's durability policy. The local backend uses temp + flush +
    // atomic rename. Returns the final physical size.
    virtual Result<std::uint64_t> put(ByteSpan data, const std::string& key) = 0;

    // Reads back all stored bytes for a key.
    virtual Result<Bytes> read(const std::string& key) const = 0;

    // Removes a stored object. Returns Ok even when absent (idempotent delete).
    virtual Status remove(const std::string& key) = 0;

    virtual Result<bool> exists(const std::string& key) const = 0;
    virtual Result<std::uint64_t> size(const std::string& key) const = 0;
    virtual Result<std::vector<std::string>> enumerate() const = 0;
    virtual Result<BackendCapacity> query_capacity() const = 0;

    // Recomputes the digest of stored bytes and compares against the expected
    // digest (when non-zero). Reports size and recomputed digest.
    virtual Result<VerifyResult> verify(const std::string& key, ContentDigest expect = {}) const = 0;

    virtual Status flush() = 0;
    virtual bool is_synthetic() const noexcept = 0;
};

// A backend factory that also exposes its descriptor for planning.
using BackendFactory = std::function<std::shared_ptr<StorageBackend>(const BackendDescriptor&)>;

}  // namespace storagefabric
