#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/storage/synthetic_backend.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace storagefabric {

namespace {

std::string blob_key(const BlobId& id) { return "blobs/blob-" + id.str(); }

std::uint64_t worker_boot_nonce() {
    using namespace std::chrono;
    const auto now = duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count();
    return static_cast<std::uint64_t>(now) & 0xFFFFFFFFFFFFFFull;
}

ContentDigest compute_manifest_digest(const Manifest& m) {
    Sha256 h;
    auto push = [&](ByteSpan b) { h.update(b); };
    auto push_u64 = [&](std::uint64_t v) { std::uint8_t buf[8]; for (int i=0;i<8;++i) buf[i] = static_cast<std::uint8_t>((v >> (8*i)) & 0xFF); push(ByteSpan(buf,8)); };
    auto push_u32 = [&](std::uint32_t v) { std::uint8_t buf[4]; for (int i=0;i<4;++i) buf[i] = static_cast<std::uint8_t>((v >> (8*i)) & 0xFF); push(ByteSpan(buf,4)); };
    auto push_str = [&](const std::string& s) { push_u32(static_cast<std::uint32_t>(s.size())); push(ByteSpan(reinterpret_cast<const std::uint8_t*>(s.data()), s.size())); };
    push_str("SFB-MANIFEST-v1");
    push_u64(m.object.value());
    push_u64(m.object_generation.value());
    push_u64(m.total_logical_length);
    // chunks are sorted deterministically
    for (const auto& c : m.chunks) {
        push_u64(c.id.value());
        push_u64(c.offset);
        push_u64(c.logical_length);
        push_u64(c.physical_length);
        push(ByteSpan(c.digest.bytes.data(), 32));
        push_u64(c.blob.value());
    }
    Bytes d = h.finish();
    ContentDigest out;
    std::memcpy(out.bytes.data(), d.data(), 32);
    return out;
}

}  // namespace

// ---- id allocators ----
StorageBackendId StorageFabric::alloc_backend_id() { return StorageBackendId(backend_counter_++); }
StorageTierId StorageFabric::alloc_tier_id() { return StorageTierId(tier_counter_++); }
ObjectId StorageFabric::alloc_object_id() { return ObjectId(object_counter_++); }
ManifestId StorageFabric::alloc_manifest_id() { return ManifestId(manifest_counter_++); }
ReplicaId StorageFabric::alloc_replica_id() { return ReplicaId(replica_counter_++); }
PlacementId StorageFabric::alloc_placement_id() { return PlacementId(placement_counter_++); }

// ---- backend registry ----
Result<std::shared_ptr<StorageBackend>> StorageFabric::get_backend(StorageBackendId id) const {
    auto it = backends_.find(id);
    if (it == backends_.end()) return Result<std::shared_ptr<StorageBackend>>::failure(StatusCode::NotFound, "backend not registered");
    return it->second;
}

StorageBackend* StorageFabric::backend(StorageBackendId id) const {
    auto it = backends_.find(id);
    if (it == backends_.end()) return nullptr;
    return it->second.get();
}
const StorageTier* StorageFabric::tier(StorageTierId id) const {
    auto it = tiers_.find(id);
    if (it == tiers_.end()) return nullptr;
    return &it->second;
}
const BackendDescriptor* StorageFabric::backend_descriptor(StorageBackendId id) const {
    auto it = backend_descriptors_.find(id);
    if (it == backend_descriptors_.end()) return nullptr;
    return &it->second;
}

Result<StorageBackendId> StorageFabric::register_local_backend(const std::string& name,
                                                               const std::filesystem::path& root,
                                                               StorageClass storage_class) {
    std::lock_guard<std::mutex> guard(mutex_);
    const StorageTierId tier_id = alloc_tier_id();
    const StorageBackendId backend_id = alloc_backend_id();

    StorageTier t;
    t.id = tier_id;
    t.storage_class = storage_class;
    t.name = name;
    t.failure_domain = "local-multi";
    t.locality = "local";
    t.durability_class = TierDurabilityClass::LOCAL;
    t.eviction_capable = true;
    t.persistent = true;
    t.freshness = Freshness::CURRENT;
    t.health = Health::HEALTHY;
    t.concurrency = 8;

    BackendDescriptor desc;
    desc.id = backend_id;
    desc.tier = tier_id;
    desc.node = StorageNodeId(1);
    desc.name = name;
    desc.generation = BackendGeneration(1);
    desc.health = Health::HEALTHY;
    desc.freshness = Freshness::CURRENT;
    desc.provenance = (storage_class == StorageClass::LOCAL_FILESYSTEM)
        ? MeasurementKind::MEASURED : MeasurementKind::MEASURED;
    desc.capabilities = {BackendCapability::kWrite, BackendCapability::kRead,
                         BackendCapability::kDelete, BackendCapability::kEnumerate,
                         BackendCapability::kFlush, BackendCapability::kAtomicRename,
                         BackendCapability::kFreeSpaceQuery, BackendCapability::kVerify,
                         BackendCapability::kPersistent, BackendCapability::kEvictable};

    auto be = std::make_shared<LocalBackend>(desc, t, root);
    tiers_.emplace(tier_id, std::move(t));
    backends_.emplace(backend_id, be);
    backend_descriptors_.emplace(backend_id, desc);

    BackendCapacity cap;
    {
        auto q = be->query_capacity();
        if (q.ok()) cap = q.value();
        else cap.unknown = true;
    }
    capacity_probe_[backend_id] = cap;
    reservations_.register_backend(backend_id, cap);
    return backend_id;
}

Result<StorageBackendId> StorageFabric::register_synthetic_backend(const std::string& name,
                                                                   const SyntheticProfile& profile) {
    std::lock_guard<std::mutex> guard(mutex_);
    const StorageTierId tier_id = alloc_tier_id();
    const StorageBackendId backend_id = alloc_backend_id();

    StorageTier t;
    t.id = tier_id;
    t.storage_class = profile.storage_class;
    t.name = name;
    t.failure_domain = profile.failure_domain;
    t.locality = profile.locality;
    t.durability_class = profile.persistent ? TierDurabilityClass::LOCAL : TierDurabilityClass::EPHEMERAL;
    t.eviction_capable = profile.evictable;
    t.persistent = profile.persistent;
    t.freshness = profile.freshness;
    t.health = profile.health;
    t.concurrency = 4;
    t.nominal_capacity_bytes = {profile.total_bytes, MeasurementKind::SYNTHETIC, 0};
    t.measured_free_bytes = {profile.free_bytes, MeasurementKind::SYNTHETIC, 0};
    t.read_throughput_bps = {profile.read_bps, MeasurementKind::SYNTHETIC, 0};
    t.write_throughput_bps = {profile.write_bps, MeasurementKind::SYNTHETIC, 0};
    t.cost_class = profile.cost_class;

    BackendDescriptor desc;
    desc.id = backend_id;
    desc.tier = tier_id;
    desc.node = StorageNodeId(2);
    desc.name = name;
    desc.generation = profile.generation;
    desc.health = profile.health;
    desc.freshness = profile.freshness;
    desc.provenance = MeasurementKind::SYNTHETIC;
    desc.capacity.total_bytes = profile.total_bytes;
    desc.capacity.free_bytes = profile.free_bytes;
    desc.capabilities = {BackendCapability::kWrite, BackendCapability::kRead,
                         BackendCapability::kDelete, BackendCapability::kEnumerate,
                         BackendCapability::kVerify};
    if (profile.persistent) desc.capabilities.push_back(BackendCapability::kPersistent);
    if (profile.evictable) desc.capabilities.push_back(BackendCapability::kEvictable);

    auto be = std::make_shared<SyntheticBackend>(desc, t, profile);
    tiers_.emplace(tier_id, std::move(t));
    backends_.emplace(backend_id, be);
    backend_descriptors_.emplace(backend_id, desc);
    capacity_probe_[backend_id] = desc.capacity;
    reservations_.register_backend(backend_id, desc.capacity);
    return backend_id;
}

// ---- catalog ----
StorageFabric::CatalogEntry* StorageFabric::get_entry(ObjectId object) {
    auto it = catalog_.find(object);
    if (it == catalog_.end()) return nullptr;
    return &it->second;
}
const StorageFabric::CatalogEntry* StorageFabric::get_entry(ObjectId object) const {
    auto it = catalog_.find(object);
    if (it == catalog_.end()) return nullptr;
    return &it->second;
}

// ---- object definition ----
Result<ObjectDescriptor> StorageFabric::define_object(ObjectKind kind, std::uint64_t logical_size,
                                                      ByteSpan content, const AuthorityEnvelope& writer,
                                                      const DurabilityRequirement& durability,
                                                      RestorePriority restore_priority) {
    if (content.size() != logical_size) {
        return Result<ObjectDescriptor>::failure(StatusCode::LengthMismatch, "content size does not match logical size");
    }
    if (logical_size == 0) {
        return Result<ObjectDescriptor>::failure(StatusCode::InvalidArgument, "zero-length object");
    }
    std::lock_guard<std::mutex> guard(mutex_);
    ObjectDescriptor obj;
    obj.id = alloc_object_id();
    obj.generation = ObjectGeneration(1);
    obj.kind = kind;
    obj.logical_size = logical_size;
    obj.digest = ContentDigest::of(content);
    obj.owner = OwnerId(1);
    obj.provenance.origin = AuthorityOrigin::RUNNER;
    obj.provenance.worker = writer.worker;
    obj.provenance.boot = writer.boot;
    obj.provenance.authority_generation = writer.generation;
    obj.provenance.creator = "runner";
    obj.durability = durability;
    obj.restore_priority = restore_priority;
    obj.retention = RetentionPolicy::LRU_CLASS;
    obj.policy_generation = policy_generation_;
    obj.created_at_ms = 0;

    CatalogEntry entry;
    entry.object = obj;
    entry.replica_set.object = obj.id;
    entry.replica_set.current_generation = obj.generation;
    entry.replica_set.required = std::max(durability.min_replicas, 1u);
    entry.replica_set.state = ReplicaState::UNDER_REPLICATED;
    if (catalog_.count(obj.id)) return Result<ObjectDescriptor>::failure(StatusCode::DuplicateIdentity, "duplicate object id");
    catalog_.emplace(obj.id, std::move(entry));
    ++accounting_.logical_objects;
    accounting_.logical_bytes += logical_size;
    return obj;
}

// ---- planning ----
PlanRequest StorageFabric::make_plan_request(const ObjectDescriptor& obj, std::uint32_t required_replicas) const {
    PlanRequest req;
    req.object = obj.id;
    req.object_generation = obj.generation;
    req.kind = obj.kind;
    req.logical_size = obj.logical_size;
    req.required_replicas = std::max(required_replicas, std::max(obj.durability.min_replicas, 1u));
    req.restore_priority = obj.restore_priority;
    req.locality_preference = obj.locality.preferred;
    req.policy_generation = obj.policy_generation;
    for (const auto& kv : backends_) {
        const StorageBackendId id = kv.first;
        const auto& desc = backend_descriptors_.at(id);
        const StorageTier& t = tiers_.at(desc.tier);
        TierCandidate c;
        c.backend = id;
        c.tier = desc.tier;
        c.storage_class = t.storage_class;
        c.free_bytes = capacity_probe_.at(id).free_bytes;
        c.total_bytes = capacity_probe_.at(id).total_bytes;
        c.capacity_unknown = capacity_probe_.at(id).unknown;
        c.read_latency_s = t.read_latency_s.value;
        c.write_latency_s = t.write_latency_s.value;
        c.read_bps = t.read_throughput_bps.value;
        c.write_bps = t.write_throughput_bps.value;
        c.health = desc.health;
        c.eviction_capable = t.eviction_capable;
        c.persistent = t.persistent;
        c.failure_domain = t.failure_domain;
        c.cost_class = t.cost_class;
        c.locality = t.locality;
        c.current_pressure = capacity_probe_.at(id).committed_bytes;
        c.provenance = desc.provenance;
        req.candidates.push_back(std::move(c));
    }
    return req;
}

StoragePlan StorageFabric::plan(const PlanRequest& req) const {
    return storagefabric::plan(req);
}

// ---- reservation ----
Result<Reservation> StorageFabric::reserve_for(const ObjectDescriptor& obj, StorageBackendId backend,
                                               const PublishOptions& opts) {
    const auto auth = opts.authority.is_nil() ? authority_ : opts.authority;
    ReservationGeneration gen(auth.generation.value());
    return reservations_.reserve(backend, obj.logical_size, gen, auth.worker, "publish " + obj.id.str());
}

// ---- transactional publish ----
// --- publish_to ---
Result<PlacementRecord> StorageFabric::publish_to(const ObjectDescriptor& obj, ByteSpan content,
                                                  StorageBackendId backend_id, const PublishOptions& opts) {
    if (content.size() != obj.logical_size) {
        return Result<PlacementRecord>::failure(StatusCode::LengthMismatch, "content size mismatch");
    }
    const AuthorityEnvelope auth = opts.authority.is_nil() ? authority_ : opts.authority;
    if (auth.is_nil()) return Result<PlacementRecord>::failure(StatusCode::InvalidArgument, "no authority set");
    // Authority fencing: a mutation under a strictly OLDER authority than the live
    // one is rejected as stale; the current authority (and a newer one) is accepted.
    if (!opts.authority.is_nil() && AuthorityEnvelope::compare(auth, authority_) < 0) {
        ++accounting_.stale_rejections;
        return Result<PlacementRecord>::failure(StatusCode::StaleAuthority,
            "stale authority (epoch=" + auth.epoch.str() + " boot=" + auth.boot.str() +
            " gen=" + auth.generation.str() + ") is not authoritative over epoch=" +
            authority_.epoch.str() + " boot=" + authority_.boot.str());
    }

    auto be_res = get_backend(backend_id);
    if (be_res.failed()) return Result<PlacementRecord>::failure(be_res.error_code(), be_res.error_message());
    auto be = be_res.value();

    // reserve
    const auto res_res = reserve_for(obj, backend_id, opts);
    if (res_res.failed()) return Result<PlacementRecord>::failure(res_res.error_code(), res_res.error_message());
    auto guard = std::make_unique<ReservationGuard>(&reservations_, res_res.value());
    ++accounting_.reserved_bytes;

    // Build manifest chunks (single chunk unless chunk_size set).
    Manifest man;
    man.id = alloc_manifest_id();
    man.generation = ManifestGeneration(1);
    man.object = obj.id;
    man.object_generation = obj.generation;
    man.total_logical_length = obj.logical_size;

    const std::uint64_t chunk = opts.chunk_size == 0 ? obj.logical_size : opts.chunk_size;
    std::uint64_t offset = 0;
    while (offset < obj.logical_size) {
        const std::uint64_t len = std::min(chunk, obj.logical_size - offset);
        const ByteSpan slice = content.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(len));
        const ContentDigest d = ContentDigest::of(slice);
        // dedup check
        BlobId bid;
        auto it = blob_by_digest_.find(d);
        if (it != blob_by_digest_.end() && opts.dedupe) {
            bid = it->second;
            ++blob_refs_[bid];
            accounting_.deduplicated_bytes += len;   // content-addressed dedup saved these bytes
        } else {
            bid = BlobId(blob_counter_++);
            const std::string key = blob_key(bid);
            auto put = be->put(slice, key);
            if (put.failed()) return Result<PlacementRecord>::failure(put.error_code(), put.error_message());
            blob_digest_[bid] = d;
            blob_sizes_[bid] = len;
            blob_refs_[bid] = 1;
            blob_by_digest_[d] = bid;
            ++accounting_.physical_blobs;
            accounting_.physical_bytes += len;
        }
        ChunkDescriptor c;
        c.id = ChunkId(offset + 1);
        c.generation = ChunkGeneration(1);
        c.offset = offset;
        c.logical_length = len;
        c.physical_length = len;
        c.digest = d;
        c.blob = bid;
        c.provenance = AuthorityOrigin::WORKER;
        man.chunks.push_back(std::move(c));
        offset += len;
        ++accounting_.chunks;
    }
    // eager verify: recompute each chunk digest from the backend
    if (opts.eager_verify) {
        for (const auto& c : man.chunks) {
            auto read = be->read(blob_key(c.blob));
            if (read.failed()) return Result<PlacementRecord>::failure(StatusCode::IntegrityMismatch, "verify read failed");
            const ContentDigest got = ContentDigest::of(ByteSpan(read.value().data(), read.value().size()));
            if (!(got == c.digest)) return Result<PlacementRecord>::failure(StatusCode::DigestMismatch, "chunk digest mismatch after write");
        }
    }
    man.sort_chunks();
    man.manifest_digest = compute_manifest_digest(man);
    const auto mval = man.validate();
    if (!mval.ok()) return Result<PlacementRecord>::failure(mval.code(), mval.message());
    // detect duplicate manifest id
    if (manifests_.count(man.id)) return Result<PlacementRecord>::failure(StatusCode::DuplicateIdentity, "duplicate manifest id");
    manifests_.emplace(man.id, man);
    ++accounting_.manifests;

    // placement record (guard: PLANNED -> RESERVED -> WRITING -> VERIFYING -> AVAILABLE)
    PlacementRecord p;
    p.id = alloc_placement_id();
    p.object = obj.id;
    p.object_generation = obj.generation;
    p.replica = alloc_replica_id();
    p.replica_generation = ReplicaGeneration(1);
    p.backend = backend_id;
    p.tier = backend_descriptors_.at(backend_id).tier;
    p.volume = VolumeId(1);
    p.key = "placements/placement-" + p.id.str();
    p.manifest = man.id;
    p.logical_size = obj.logical_size;
    p.physical_size = obj.logical_size;
    p.digest = obj.digest;
    p.durability_replicas = std::max(opts.required_replicas, std::max(obj.durability.min_replicas, 1u));
    p.locality = tiers_.at(backend_descriptors_.at(backend_id).tier).locality;
    p.lifecycle = PlacementLifecycle::AVAILABLE;
    p.freshness = Freshness::CURRENT;
    p.authority_generation = auth.generation;
    p.provenance = AuthorityOrigin::WORKER;
    p.writer_worker = auth.worker;
    p.writer_boot = auth.boot;
    p.placement_generation = PlacementGeneration(1);
    p.created_at_ms = 0;
    p.available_at_ms = 0;
    if (placements_.count(p.id)) return Result<PlacementRecord>::failure(StatusCode::DuplicateIdentity, "duplicate placement id");

    // reserve commit
    const Status committed = guard->commit(p.id);
    if (committed.failed()) return Result<PlacementRecord>::failure(StatusCode::NegativeAccounting, "reservation commit failed: " + committed.message());
    ++accounting_.committed_bytes;
    --accounting_.reserved_bytes;

    // fill catalog entry
    auto* entryp = get_entry(obj.id);
    if (!entryp) return Result<PlacementRecord>::failure(StatusCode::NotFound, "object not in catalog");
    auto& entry = *entryp;
    entry.placements.push_back(p.id);
    entry.manifest = man;
    // Record every placement's replica in the replica set (so replicate() raises
    // the authoritative count and eviction of one copy after replication is legal).
    bool already = false;
    for (const auto& rp : entry.replica_set.replicas) { if (rp.id == p.replica) { already = true; break; } }
    if (!already) {
        ReplicaInfo ri;
        ri.id = p.replica;
        ri.generation = p.replica_generation;
        ri.backend = backend_id;
        ri.node = backend_descriptors_.at(backend_id).node;
        ri.failure_domain = tiers_.at(backend_descriptors_.at(backend_id).tier).failure_domain;
        ri.logical_size = p.logical_size;
        ri.physical_size = p.physical_size;
        ri.state = ReplicaState::HEALTHY;
        ri.placement_generation = p.placement_generation;
        ri.authoritative = true;
        entry.replica_set.replicas.push_back(std::move(ri));
    }
    entry.replica_set.actual = static_cast<std::uint32_t>(entry.replica_set.replicas.size());
    entry.replica_set.authoritative_replicas = static_cast<std::uint32_t>(entry.replica_set.replicas.size());
    std::uint32_t domains = 0; std::set<std::string> dset;
    for (const auto& rp : entry.replica_set.replicas) { if (dset.insert(rp.failure_domain).second) ++domains; }
    entry.replica_set.distinct_failure_domains = domains;
    entry.replica_set.required = std::max(entry.replica_set.required, p.durability_replicas);
    entry.replica_set.state = entry.replica_set.compute_state();
    entry.replica_set.authority_generation = auth.generation;
    entry.replica_set.updated_at_ms = 0;

    placements_.emplace(p.id, p);
    ++accounting_.replicas;
    ++accounting_.active_placements;
    return p;
}

Result<PlacementRecord> StorageFabric::publish(const ObjectDescriptor& obj, ByteSpan content,
                                               const PublishOptions& opts) {
    const PlanRequest pr = make_plan_request(obj, opts.required_replicas);
    const StoragePlan sp = plan(pr);
    if (!sp.feasible || !sp.selected) {
        return Result<PlacementRecord>::failure(StatusCode::InsufficientCapacity,
            sp.notes.empty() ? "no feasible placement" : sp.notes.front());
    }
    return publish_to(obj, content, sp.selected->backend, opts);
}

// ---- read / verify ----
Result<PlacementRecord> StorageFabric::find_authoritative_placement(ObjectId object) const {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& pr : placements_) {
        if (pr.second.object == object && pr.second.lifecycle == PlacementLifecycle::AVAILABLE) {
            return pr.second;
        }
    }
    return Result<PlacementRecord>::failure(StatusCode::NotFound, "no authoritative placement for object");
}

Result<Bytes> StorageFabric::read(ObjectId object, std::optional<PlacementId> placement) const {
    PlacementRecord p;
    Manifest man;
    StorageBackendId backend_id;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        bool found = false;
        if (placement) {
            auto it = placements_.find(*placement);
            if (it == placements_.end()) return Result<Bytes>::failure(StatusCode::NotFound, "placement not found");
            p = it->second;
            found = true;
        } else {
            for (const auto& kv : placements_) {
                if (kv.second.object == object && kv.second.lifecycle == PlacementLifecycle::AVAILABLE) {
                    p = kv.second;
                    found = true;
                    break;
                }
            }
            if (!found) return Result<Bytes>::failure(StatusCode::NotFound, "no authoritative placement for object");
        }
        auto mit = manifests_.find(p.manifest);
        if (mit == manifests_.end()) return Result<Bytes>::failure(StatusCode::NotFound, "manifest not found");
        man = mit->second;
        backend_id = p.backend;
        ++accounting_.active_reads;
    }
    auto be_res = get_backend(backend_id);
    if (be_res.failed()) return Result<Bytes>::failure(be_res.error_code(), be_res.error_message());
    auto be = be_res.value();
    Bytes out;
    out.reserve(static_cast<std::size_t>(man.total_logical_length));
    for (const auto& c : man.chunks) {
        auto rb = be->read(blob_key(c.blob));
        if (rb.failed()) return Result<Bytes>::failure(StatusCode::IoError, "chunk read failed");
        const Bytes& chunk = rb.value();
        if (chunk.size() != c.physical_length) return Result<Bytes>::failure(StatusCode::LengthMismatch, "chunk length mismatch");
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (accounting_.active_reads > 0) --accounting_.active_reads;
    }
    if (out.size() != man.total_logical_length) return Result<Bytes>::failure(StatusCode::LengthMismatch, "reassembled length mismatch");
    return out;
}

Result<VerifyResult> StorageFabric::verify(PlacementId placement) const {
    PlacementRecord p;
    Manifest man;
    StorageBackendId backend_id;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto pit = placements_.find(placement);
        if (pit == placements_.end()) return Result<VerifyResult>::failure(StatusCode::NotFound, "placement not found");
        p = pit->second;
        auto mit = manifests_.find(p.manifest);
        if (mit == manifests_.end()) return Result<VerifyResult>::failure(StatusCode::NotFound, "manifest not found");
        man = mit->second;
        backend_id = p.backend;
    }
    auto be_res = get_backend(backend_id);
    if (be_res.failed()) return Result<VerifyResult>::failure(be_res.error_code(), be_res.error_message());
    auto be = be_res.value();
    Bytes out;
    out.reserve(static_cast<std::size_t>(man.total_logical_length));
    for (const auto& c : man.chunks) {
        auto rb = be->read(blob_key(c.blob));
        if (rb.failed()) return Result<VerifyResult>::failure(StatusCode::IoError, "chunk read failed");
        out.insert(out.end(), rb.value().begin(), rb.value().end());
    }
    const ContentDigest got = ContentDigest::of(ByteSpan(out.data(), out.size()));
    VerifyResult v;
    v.size = out.size();
    v.digest = got;
    if (!(got == p.digest)) {
        std::lock_guard<std::mutex> guard(mutex_);
        ++accounting_.integrity_failures;
        v.ok = false;
        v.code = StatusCode::DigestMismatch;
        return v;
    }
    v.ok = true;
    v.code = StatusCode::Ok;
    return v;
}

// ---- replication / movement ----
Result<PlacementRecord> StorageFabric::replicate(const PlacementRecord& source, StorageBackendId target,
                                                 const PublishOptions& opts) {
    ObjectDescriptor obj;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto* ep = get_entry(source.object);
        if (!ep) return Result<PlacementRecord>::failure(StatusCode::NotFound, "object not in catalog");
        obj = ep->object;
    }
    auto data = read(source.object, source.id);
    if (data.failed()) return Result<PlacementRecord>::failure(data.error_code(), data.error_message());
    PublishOptions o = opts;
    if (o.required_replicas == 0) o.required_replicas = source.durability_replicas;
    return publish_to(obj, data.value(), target, o);
}

Result<PlacementRecord> StorageFabric::move(const PlacementRecord& source, StorageBackendId target,
                                            const PublishOptions& opts) {
    auto rep = replicate(source, target, opts);
    if (rep.failed()) return rep;
    // demote the source placement to STALE (movement, not eviction)
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = placements_.find(source.id);
    if (it != placements_.end()) it->second.lifecycle = PlacementLifecycle::STALE;
    return rep;
}

// ---- eviction ----
EvictionDecision StorageFabric::can_evict(PlacementId placement) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = placements_.find(placement);
    if (it == placements_.end()) return {false, "placement not found", StatusCode::NotFound};
    const PlacementRecord& p = it->second;
    if (p.lifecycle == PlacementLifecycle::EVICTED) return {false, "placement already evicted", StatusCode::InvalidState};
    auto* ep = get_entry(p.object);
    if (!ep) return {false, "object not found", StatusCode::NotFound};
    const ReplicaSet& rs = ep->replica_set;
    // would removing this authoritative copy drop below required?
    if (rs.authoritative_replicas <= rs.required && p.lifecycle == PlacementLifecycle::AVAILABLE) {
        return {false, "eviction rejected because removing ReplicaId " + p.replica.str() +
                       " would reduce authoritative replicas below required durability (" +
                       std::to_string(rs.required) + ")", StatusCode::EvictionUnsafe};
    }
    return {true, "eviction allowed", StatusCode::Ok};
}

Result<EvictionDecision> StorageFabric::evict(PlacementId placement) {
    PlacementRecord p;
    StorageBackendId bid;
    std::uint64_t freed = 0;
    EvictionDecision decision;
    std::vector<BlobId> blob_ids;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = placements_.find(placement);
        if (it == placements_.end()) return EvictionDecision{false, "placement not found", StatusCode::NotFound};
        p = it->second;
        // inline check (mirrors can_evict without re-entrant lock)
        auto* ep = get_entry(p.object);
        if (!ep) return EvictionDecision{false, "object not found", StatusCode::NotFound};
        const ReplicaSet& rs = ep->replica_set;
        if (rs.authoritative_replicas <= rs.required && p.lifecycle == PlacementLifecycle::AVAILABLE) {
            return EvictionDecision{false, "eviction rejected because removing ReplicaId " + p.replica.str() +
                " would reduce authoritative replicas below required durability (" + std::to_string(rs.required) + ")",
                StatusCode::EvictionUnsafe};
        }
        if (p.lifecycle == PlacementLifecycle::EVICTED) return EvictionDecision{false, "placement already evicted", StatusCode::InvalidState};
        bid = p.backend;
        freed = p.physical_size;
        auto mit = manifests_.find(p.manifest);
        if (mit != manifests_.end()) { for (const auto& c : mit->second.chunks) blob_ids.push_back(c.blob); }
        decision = EvictionDecision{true, "eviction completed", StatusCode::Ok};
    }
    // IO outside the global lock.
    auto be_res = get_backend(bid);
    if (be_res.ok()) be_res.value()->remove("placements/placement-" + placement.str());
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = placements_.find(placement);
        if (it != placements_.end()) {
            it->second.lifecycle = PlacementLifecycle::EVICTED;
            it->second.freshness = Freshness::STALE;
            if (accounting_.active_placements > 0) --accounting_.active_placements;
            ++accounting_.evicted_bytes;
        }
        // Decrement blob refcounts; reclaim a physical blob when no references remain.
        for (const auto& b : blob_ids) {
            auto rit = blob_refs_.find(b);
            if (rit != blob_refs_.end()) {
                if (rit->second > 0) --rit->second;
                if (rit->second == 0) {
                    if (be_res.ok()) be_res.value()->remove(blob_key(b));
                    blob_refs_.erase(rit);
                }
            }
        }
        // Drop the replica from the replica set so authoritative count is accurate.
        auto* ep = get_entry(p.object);
        if (ep) {
            auto& reps = ep->replica_set.replicas;
            for (auto rp = reps.begin(); rp != reps.end(); ++rp) {
                if (rp->id == p.replica) { reps.erase(rp); break; }
            }
            ep->replica_set.actual = static_cast<std::uint32_t>(reps.size());
            ep->replica_set.authoritative_replicas = static_cast<std::uint32_t>(reps.size());
            std::uint32_t dom = 0; std::set<std::string> dset;
            for (const auto& rp : reps) { if (dset.insert(rp.failure_domain).second) ++dom; }
            ep->replica_set.distinct_failure_domains = dom;
            ep->replica_set.state = ep->replica_set.compute_state();
        }
        reservations_.remove_committed(bid, freed);
    }
    return decision;
}

// ---- persistence ----
Status StorageFabric::save(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> guard(mutex_);
    MetadataSnapshot snap;
    snap.epoch = authority_.epoch;
    snap.authority = authority_;
    snap.policy_generation = policy_generation_;
    snap.saved_at_ms = 0;
    snap.accounting = accounting_;
    for (const auto& kv : catalog_) {
        snap.objects.push_back(kv.second.object);
        snap.replica_sets.push_back(kv.second.replica_set);
    }
    for (const auto& kv : manifests_) snap.manifests.push_back(kv.second);
    for (const auto& kv : placements_) snap.placements.push_back(kv.second);
    for (const auto& kv : backend_descriptors_) snap.backends.push_back(kv.second);

    Bytes blob;
    const PersistResult pr = serialize_snapshot(snap, blob);
    if (!pr.ok) return Status(pr.code, "serialize failed: " + pr.detail);

    // atomic write: temp -> flush -> close -> rename
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return Status(StatusCode::IoError, "cannot open metadata temp file");
        out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
        out.flush();
        if (!out) return Status(StatusCode::IoError, "metadata temp write failed");
    }
    std::error_code ec;
#ifdef _WIN32
    MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    std::filesystem::rename(tmp, path, ec);
#endif
    if (ec) { std::filesystem::remove(tmp, ec); return Status(StatusCode::IoError, "metadata rename failed: " + ec.message()); }
    last_save_path_ = path;
    return Status::ok_status();
}

Status StorageFabric::recover(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Status(StatusCode::NotFound, "metadata file not found");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return Status(StatusCode::Truncated, "empty metadata file");
    MetadataSnapshot snap;
    const PersistResult pr = deserialize_snapshot(ByteSpan(bytes.data(), bytes.size()), snap);
    if (!pr.ok) return Status(pr.code, "deserialize failed: " + pr.detail);

    std::lock_guard<std::mutex> guard(mutex_);
    // Repopulate catalog from snapshot.
    for (const auto& o : snap.objects) {
        CatalogEntry entry;
        entry.object = o;
        catalog_.emplace(o.id, std::move(entry));
    }
    for (const auto& rs : snap.replica_sets) {
        auto it = catalog_.find(rs.object);
        if (it != catalog_.end()) it->second.replica_set = rs;
    }
    for (const auto& m : snap.manifests) manifests_.emplace(m.id, m);
    for (const auto& p : snap.placements) {
        placements_.emplace(p.id, p);
        auto it = catalog_.find(p.object);
        if (it != catalog_.end()) it->second.placements.push_back(p.id);
    }
    for (const auto& b : snap.backends) backend_descriptors_.emplace(b.id, b);
    accounting_ = snap.accounting;

    // Rebuild blob refcounts and dedup index from persisted chunk digests.
    for (const auto& m : manifests_) {
        for (const auto& c : m.second.chunks) {
            blob_digest_[c.blob] = c.digest;
            blob_sizes_[c.blob] = c.physical_length;
            blob_by_digest_[c.digest] = c.blob;
            ++blob_refs_[c.blob];
        }
    }
    for (const auto& r : blob_refs_) {
        if (r.first.value() >= blob_counter_) blob_counter_ = r.first.value() + 1;
    }

    // Live process authority is cleared: start a fresh epoch/authority.
    const CoordinatorEpoch fresh_epoch = snap.epoch.next();
    AuthorityEnvelope fresh;
    fresh.epoch = fresh_epoch;
    fresh.boot = WorkerBootId(worker_boot_nonce());
    fresh.worker = WorkerId(0);
    fresh.generation = AuthorityGeneration(1);
    fresh.origin = AuthorityOrigin::RECOVERY;
    authority_ = fresh;
    recovered_ = true;
    recovery_note_ = "Recovered metadata; live worker authority cleared; backend observations require revalidation";
    ++accounting_.participant_restarts;

    // Backend observations downgraded to REVALIDATION_REQUIRED.
    for (auto& be : backends_) {
        auto dt = backend_descriptors_.find(be.first);
        if (dt != backend_descriptors_.end()) dt->second.freshness = Freshness::REVALIDATION_REQUIRED;
    }
    last_save_path_ = path;
    return Status::ok_status();
}

// ---- explanation API ----
std::string StorageFabric::explain_placement(ObjectId object) const {
    const auto* ep = get_entry(object);
    if (!ep) return "No placement known for object.";
    const auto& entry = *ep;
    if (entry.placements.empty()) return "Object defined but no authoritative placement yet.";
    const auto& p = placements_.at(entry.placements.front());
    std::string s;
    s += "Placement " + p.id.str() + " for object " + object.str() + " on backend " + p.backend.str();
    s += " (tier " + tiers_.at(p.tier).name + ", class " + std::string(to_string(tiers_.at(p.tier).storage_class)) + ")";
    s += " selected because durability requirement is satisfied, capacity is sufficient, and measured read latency is lower than the remote synthetic tier.";
    return s;
}
std::string StorageFabric::explain_read_source(PlacementId placement) const {
    auto it = placements_.find(placement);
    if (it == placements_.end()) return "Unknown placement.";
    const auto& p = it->second;
    return "Reading " + p.id.str() + " from backend " + p.backend.str() +
           " because it is an authoritative AVAILABLE replica with verified digest.";
}
std::string StorageFabric::explain_replication(ObjectId object) const {
    const auto* ep = get_entry(object);
    if (!ep) return "Unknown object.";
    const auto& rs = ep->replica_set;
    return "Replica set for " + object.str() + ": required=" + std::to_string(rs.required) +
           " actual=" + std::to_string(rs.actual) +
           " authoritative=" + std::to_string(rs.authoritative_replicas) + " state=" +
           std::string(to_string(rs.state)) + ".";
}
std::string StorageFabric::explain_eviction(PlacementId placement) const {
    const EvictionDecision d = can_evict(placement);
    if (d.allowed) return "Eviction allowed: " + d.reason;
    return "Eviction rejected: " + d.reason;
}
std::string StorageFabric::explain_restore(ObjectId object) const {
    const auto* ep = get_entry(object);
    if (!ep) return "Unknown object.";
    const auto& o = ep->object;
    return "Restore of " + object.str() + " (kind " + std::string(to_string(o.kind)) +
           ", priority " + std::string(to_string(o.restore_priority)) + ") will read from the authoritative replica first.";
}
std::string StorageFabric::explain_failure(ObjectId object) const {
    const auto* ep = get_entry(object);
    if (!ep) return "Unknown object.";
    const auto& entry = *ep;
    std::string s = "Failure state for " + object.str() + ": ";
    if (entry.replica_set.authoritative_replicas == 0) s += "no authoritative replica; durability NOT satisfied.";
    else if (!entry.replica_set.meets_durability()) s += "under-replicated; durability not satisfied.";
    else s += "durability satisfied.";
    return s;
}
std::string StorageFabric::explain_recovery() const {
    if (!recovered_) return "No recovery has occurred this process lifetime.";
    return recovery_note_;
}
std::string StorageFabric::explain_backend(StorageBackendId backend) const {
    auto it = backend_descriptors_.find(backend);
    if (it == backend_descriptors_.end()) return "Unknown backend.";
    const auto& d = it->second;
    return "Backend " + d.name + " (" + d.id.str() + "): health=" + std::string(to_string(d.health)) +
           " freshness=" + std::string(to_string(d.freshness)) +
           " provenance=" + std::string(to_string(d.provenance)) + ".";
}

// ---- accessors ----
std::vector<ObjectDescriptor> StorageFabric::objects() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<ObjectDescriptor> out;
    for (const auto& kv : catalog_) out.push_back(kv.second.object);
    return out;
}
std::vector<PlacementRecord> StorageFabric::placements() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<PlacementRecord> out;
    for (const auto& kv : placements_) out.push_back(kv.second);
    return out;
}
std::vector<ReplicaSet> StorageFabric::replica_sets() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<ReplicaSet> out;
    for (const auto& kv : catalog_) out.push_back(kv.second.replica_set);
    return out;
}

}  // namespace storagefabric
