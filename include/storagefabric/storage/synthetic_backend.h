#pragma once
// Storage Fabric - deterministic synthetic backend.
// Models an unavailable/remote/object/distributed tier in memory with explicit
// SYNTHETIC provenance. It enforces a capacity ceiling, can be marked degraded
// or unavailable, models asymmetric read/write behavior, and never claims to be
// a real physical tier.

#include <map>
#include <string>
#include <cstdint>
#include <mutex>

#include "storagefabric/storage/backend.h"

namespace storagefabric {

struct SyntheticProfile {
    StorageClass storage_class{StorageClass::SYNTHETIC_REMOTE};
    std::uint64_t total_bytes{1024ULL * 1024 * 1024};
    std::uint64_t free_bytes{1024ULL * 1024 * 1024};
    double read_latency_s{0.001};
    double write_latency_s{0.001};
    double read_bps{100.0 * 1024 * 1024};
    double write_bps{50.0 * 1024 * 1024};
    Health health{Health::HEALTHY};
    bool degraded{false};
    bool unavailable{false};
    bool evictable{true};
    bool persistent{false};
    std::string failure_domain{"synthetic-node"};
    std::string cost_class{"synthetic-remote"};
    std::string locality{"synthetic"};
    BackendGeneration generation;       // backend generation rollover support
    Freshness freshness{Freshness::CURRENT};
};

class SyntheticBackend final : public StorageBackend {
public:
    SyntheticBackend(BackendDescriptor descriptor, StorageTier tier, SyntheticProfile profile);

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
    bool is_synthetic() const noexcept override { return true; }

    // Mutation of the profile at runtime (used by synthetic scenarios).
    void set_health(Health h, bool degraded, bool unavailable);
    void set_freshness(Freshness f);
    void advance_generation() { profile_.generation = profile_.generation.next(); }

    const SyntheticProfile& profile() const noexcept { return profile_; }

private:
    BackendDescriptor desc_;
    StorageTier tier_;
    SyntheticProfile profile_;
    mutable std::mutex io_lock_;
    mutable std::map<std::string, Bytes> store_;
    std::uint64_t used_bytes_{0};

    Status ensure_available(std::string_view op) const;
};

}  // namespace storagefabric
