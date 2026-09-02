#pragma once
// Storage Fabric - real local filesystem backend.
// Uses a dedicated root directory. Writes go through a temp file, are flushed
// (real flush-to-disk where the platform provides it), then atomically renamed
// into place. Keys are governed and validated; never absolute, never parent
// traversal. This backend never recursively operates outside its root.

#include <filesystem>
#include <mutex>

#include "storagefabric/storage/backend.h"

namespace storagefabric {

class LocalBackend final : public StorageBackend {
public:
    // 'root' is a dedicated (possibly created) test/working directory.
    LocalBackend(BackendDescriptor descriptor, StorageTier tier, std::filesystem::path root);
    ~LocalBackend() override = default;

    StorageBackendId id() const noexcept override { return desc_.id; }
    const BackendDescriptor& descriptor() const noexcept override { return desc_; }
    const StorageTier& tier() const noexcept override { return tier_; }

    Result<std::uint64_t> put(ByteSpan data, const std::string& key) override;
    Result<Bytes> read(const std::string& key) const override;
    Status remove(const std::string& key) override;
    Result<bool> exists(const std::string& key) const override;
    Result<std::uint64_t> size(const std::string& key) const override;
    Result<std::vector<std::string>> enumerate() const override;
    Result<BackendCapacity> query_capacity() const override;
    Result<VerifyResult> verify(const std::string& key, ContentDigest expect = {}) const override;
    Status flush() override;
    bool is_synthetic() const noexcept override { return false; }

private:
    // Resolves a validated governed key to a path inside root, returning the
    // fully canonicalized path. Returns an error status for an unsafe key.
    Result<std::filesystem::path> resolve_key(const std::string& key) const;
    // Loads the whole file into a byte vector (bounded by the caller's policy).
    Result<Bytes> read_file(const std::filesystem::path& p) const;

    BackendDescriptor desc_;
    StorageTier tier_;
    std::filesystem::path root_;
    mutable std::mutex io_lock_;
};

}  // namespace storagefabric
