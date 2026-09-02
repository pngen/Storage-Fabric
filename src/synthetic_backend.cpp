#include "storagefabric/storage/synthetic_backend.h"

#include <algorithm>

namespace storagefabric {

SyntheticBackend::SyntheticBackend(BackendDescriptor descriptor, StorageTier tier, SyntheticProfile profile)
    : desc_(std::move(descriptor)), tier_(std::move(tier)), profile_(std::move(profile)) {
    if (desc_.health == Health::UNKNOWN) desc_.health = profile_.health;
    if (desc_.provenance == MeasurementKind::UNKNOWN) desc_.provenance = MeasurementKind::SYNTHETIC;
    desc_.capacity.total_bytes = profile_.total_bytes;
    desc_.capacity.free_bytes = profile_.free_bytes;
    desc_.capacity.unknown = false;
}

Status SyntheticBackend::ensure_available(std::string_view op) const {
    if (profile_.unavailable) return Status(StatusCode::BackendUnavailable,
                                            std::string(op) + " rejected: synthetic backend unavailable");
    if (profile_.degraded && op != "read") {
        // degraded backend fails writes; reads still allowed
        return Status(StatusCode::BackendDegraded, std::string(op) + " rejected: synthetic backend degraded");
    }
    return Status::ok_status();
}

Result<std::uint64_t> SyntheticBackend::put(ByteSpan data, const std::string& key) {
    std::lock_guard<std::mutex> guard(io_lock_);
    const Status s = ensure_available("write");
    if (s.failed()) return s;

    auto it = store_.find(key);
    const std::uint64_t old = (it == store_.end()) ? 0 : static_cast<std::uint64_t>(it->second.size());
    const std::uint64_t net = static_cast<std::uint64_t>(data.size()) - old;
    if (used_bytes_ + net > profile_.total_bytes) {
        return Status(StatusCode::InsufficientCapacity, "synthetic capacity exhausted");
    }
    Bytes copy(data.begin(), data.end());
    if (it == store_.end()) {
        store_.emplace(key, std::move(copy));
    } else {
        it->second = std::move(copy);
    }
    used_bytes_ += net;
    return static_cast<std::uint64_t>(data.size());
}

Result<Bytes> SyntheticBackend::read(const std::string& key) const {
    std::lock_guard<std::mutex> guard(io_lock_);
    const Status s = ensure_available("read");
    if (s.failed()) return s;
    auto it = store_.find(key);
    if (it == store_.end()) return Status(StatusCode::NotFound, "key not present in synthetic backend");
    return it->second;
}

Status SyntheticBackend::remove(const std::string& key) {
    std::lock_guard<std::mutex> guard(io_lock_);
    const Status s = ensure_available("delete");
    if (s.failed()) return s;
    auto it = store_.find(key);
    if (it != store_.end()) {
        used_bytes_ -= static_cast<std::uint64_t>(it->second.size());
        store_.erase(it);
    }
    return Status::ok_status();
}

Result<bool> SyntheticBackend::exists(const std::string& key) const {
    std::lock_guard<std::mutex> guard(io_lock_);
    return store_.find(key) != store_.end();
}

Result<std::uint64_t> SyntheticBackend::size(const std::string& key) const {
    std::lock_guard<std::mutex> guard(io_lock_);
    auto it = store_.find(key);
    if (it == store_.end()) return Status(StatusCode::NotFound, "key not present in synthetic backend");
    return static_cast<std::uint64_t>(it->second.size());
}

Result<std::vector<std::string>> SyntheticBackend::enumerate() const {
    std::lock_guard<std::mutex> guard(io_lock_);
    std::vector<std::string> out;
    out.reserve(store_.size());
    for (const auto& kv : store_) out.push_back(kv.first);
    return out;
}

Result<BackendCapacity> SyntheticBackend::query_capacity() const {
    BackendCapacity cap;
    cap.total_bytes = profile_.total_bytes;
    cap.free_bytes = profile_.free_bytes;
    cap.reserved_bytes = 0;
    cap.committed_bytes = used_bytes_;
    cap.unknown = false;
    return cap;
}

Result<VerifyResult> SyntheticBackend::verify(const std::string& key, ContentDigest expect) const {
    auto data_res = read(key);
    if (data_res.failed()) return Status(data_res.error_code(), data_res.error_message());
    const Bytes& data = data_res.value();
    VerifyResult v;
    v.size = data.size();
    v.digest = ContentDigest::of(ByteSpan(data.data(), data.size()));
    if (!expect.is_zero() && !(v.digest == expect)) {
        v.ok = false;
        v.code = StatusCode::DigestMismatch;
        return v;
    }
    v.ok = true;
    v.code = StatusCode::Ok;
    return v;
}

Status SyntheticBackend::flush() {
    return Status::ok_status();
}

void SyntheticBackend::set_health(Health h, bool degraded, bool unavailable) {
    std::lock_guard<std::mutex> guard(io_lock_);
    profile_.health = h;
    profile_.degraded = degraded;
    profile_.unavailable = unavailable;
    desc_.health = h;
}

void SyntheticBackend::set_freshness(Freshness f) {
    profile_.freshness = f;
    desc_.freshness = f;
}

}  // namespace storagefabric
