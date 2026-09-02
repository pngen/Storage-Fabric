#include "storagefabric/core/persist.h"

#include <cstring>
#include <limits>
#include <unordered_set>

namespace storagefabric {

namespace {

bool enc_digest(ByteWriter& w, const ContentDigest& d) {
    w.put_blob(ByteSpan(d.bytes.data(), 32));
    return true;
}
bool dec_digest(ByteReader& r, ContentDigest& d) {
    ByteSpan b;
    if (!r.read_blob(b) || b.size() != 32) return false;
    std::memcpy(d.bytes.data(), b.data(), 32);
    return true;
}

bool enc_str(ByteWriter& w, const std::string& s) {
    if (s.size() > kMetaMaxString) return false;
    w.put_blob(ByteSpan(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    return true;
}
bool dec_str(ByteReader& r, std::string& s) {
    ByteSpan b;
    if (!r.read_blob(b) || b.size() > kMetaMaxString) return false;
    s.assign(reinterpret_cast<const char*>(b.data()), b.size());
    return true;
}
template <typename T>
bool enc_g(ByteWriter& w, const T& v) { w.put_u64(v.value()); return true; }
template <typename T>
bool dec_g(ByteReader& r, T& v) {
    std::uint64_t x; if (!r.read_u64(x)) return false; v = T(x); return true;
}

template <typename E>
bool enc_enum(ByteWriter& w, E e) { w.put_u8(static_cast<std::uint8_t>(e)); return true; }
template <typename E, uint8_t Max>
bool dec_enum(ByteReader& r, E& e) {
    std::uint8_t b; if (!r.read_u8(b)) return false;
    if (b > Max) return false;
    e = static_cast<E>(b); return true;
}
bool enc_i64(ByteWriter& w, std::int64_t v) { w.put_u64(static_cast<std::uint64_t>(v)); return true; }
bool dec_i64(ByteReader& r, std::int64_t& v) {
    std::uint64_t x; if (!r.read_u64(x)) return false; v = static_cast<std::int64_t>(x); return true;
}

bool enc_authority(ByteWriter& w, const AuthorityEnvelope& a) {
    return enc_g(w, a.epoch) && enc_g(w, a.boot) && enc_g(w, a.worker) &&
           enc_g(w, a.generation) && enc_enum(w, a.origin);
}
bool dec_authority(ByteReader& r, AuthorityEnvelope& a) {
    return dec_g(r, a.epoch) && dec_g(r, a.boot) && dec_g(r, a.worker) &&
           dec_g(r, a.generation) && dec_enum<AuthorityOrigin, 5>(r, a.origin);
}

bool enc_object(ByteWriter& w, const ObjectDescriptor& o) {
    if (!enc_g(w, o.id) || !enc_g(w, o.generation) || !enc_enum(w, o.kind) ||
        !w.put_u64(o.logical_size) || !enc_digest(w, o.digest) || !enc_g(w, o.owner)) return false;
    if (!enc_enum(w, o.provenance.origin) || !enc_g(w, o.provenance.worker) ||
        !enc_g(w, o.provenance.boot) || !enc_g(w, o.provenance.authority_generation) ||
        !enc_str(w, o.provenance.creator)) return false;
    if (!w.put_u32(o.durability.min_replicas) || !enc_enum(w, o.restore_priority) ||
        !enc_enum(w, o.locality.preferred) || !enc_str(w, o.locality.node_hint) ||
        !enc_enum(w, o.retention) || !enc_g(w, o.policy_generation)) return false;
    if (o.dependencies.size() > kMetaMaxRecords) return false;
    w.put_u32(static_cast<std::uint32_t>(o.dependencies.size()));
    for (const auto& dep : o.dependencies) { if (!enc_g(w, dep)) return false; }
    if (o.compatibility.size() > kMetaMaxRecords) return false;
    w.put_u32(static_cast<std::uint32_t>(o.compatibility.size()));
    for (const auto& kv : o.compatibility) { if (!enc_str(w, kv.first) || !enc_str(w, kv.second)) return false; }
    return enc_i64(w, o.created_at_ms);
}
bool dec_object(ByteReader& r, ObjectDescriptor& o) {
    if (!dec_g(r, o.id)) return false;
    if (!dec_g(r, o.generation)) return false;
    std::uint8_t kind; if (!r.read_u8(kind)) return false;
    if (kind > 11) return false; o.kind = static_cast<ObjectKind>(kind);
    if (!r.read_u64(o.logical_size)) return false;
    if (!dec_digest(r, o.digest)) return false;
    if (!dec_g(r, o.owner)) return false;
    std::uint8_t porigin; if (!r.read_u8(porigin)) return false;
    o.provenance.origin = static_cast<AuthorityOrigin>(porigin);
    if (!dec_g(r, o.provenance.worker) || !dec_g(r, o.provenance.boot) ||
        !dec_g(r, o.provenance.authority_generation) || !dec_str(r, o.provenance.creator)) return false;
    if (!r.read_u32(o.durability.min_replicas)) return false;
    std::uint8_t rprio, locpref, retention;
    if (!r.read_u8(rprio)) return false; o.restore_priority = static_cast<RestorePriority>(rprio);
    if (!r.read_u8(locpref)) return false; o.locality.preferred = static_cast<StorageClass>(locpref);
    if (!dec_str(r, o.locality.node_hint)) return false;
    if (!r.read_u8(retention)) return false; o.retention = static_cast<RetentionPolicy>(retention);
    if (!dec_g(r, o.policy_generation)) return false;
    std::uint32_t ndeps; if (!r.read_u32(ndeps) || ndeps > kMetaMaxRecords) return false;
    o.dependencies.resize(ndeps);
    for (auto& dep : o.dependencies) { if (!dec_g(r, dep)) return false; }
    std::uint32_t ncompat; if (!r.read_u32(ncompat) || ncompat > kMetaMaxRecords) return false;
    for (std::uint32_t i = 0; i < ncompat; ++i) {
        std::string k, v; if (!dec_str(r, k) || !dec_str(r, v)) return false;
        if (o.compatibility.count(k)) return false;
        o.compatibility.emplace(std::move(k), std::move(v));
    }
    return dec_i64(r, o.created_at_ms);
}

bool enc_manifest(ByteWriter& w, const Manifest& m) {
    if (!enc_g(w, m.id) || !enc_g(w, m.generation) || !enc_g(w, m.object) ||
        !enc_g(w, m.object_generation) || !w.put_u64(m.total_logical_length) ||
        !enc_digest(w, m.manifest_digest)) return false;
    if (m.chunks.size() > kMetaMaxRecords) return false;
    w.put_u32(static_cast<std::uint32_t>(m.chunks.size()));
    for (const auto& c : m.chunks) {
        if (!enc_g(w, c.id) || !enc_g(w, c.generation)) return false;
        w.put_u64(c.offset); w.put_u64(c.logical_length); w.put_u64(c.physical_length);
        if (!enc_digest(w, c.digest) || !enc_g(w, c.blob) || !enc_enum(w, c.provenance)) return false;
    }
    return true;
}
bool dec_manifest(ByteReader& r, Manifest& m) {
    if (!dec_g(r, m.id) || !dec_g(r, m.generation) || !dec_g(r, m.object) ||
        !dec_g(r, m.object_generation)) return false;
    if (!r.read_u64(m.total_logical_length)) return false;
    if (!dec_digest(r, m.manifest_digest)) return false;
    std::uint32_t nchunks; if (!r.read_u32(nchunks) || nchunks > kMetaMaxRecords) return false;
    m.chunks.reserve(nchunks);
    for (std::uint32_t i = 0; i < nchunks; ++i) {
        ChunkDescriptor c;
        if (!dec_g(r, c.id) || !dec_g(r, c.generation)) return false;
        if (!r.read_u64(c.offset) || !r.read_u64(c.logical_length) || !r.read_u64(c.physical_length)) return false;
        if (!dec_digest(r, c.digest) || !dec_g(r, c.blob)) return false;
        if (!dec_enum<AuthorityOrigin, 5>(r, c.provenance)) return false;
        m.chunks.push_back(std::move(c));
    }
    return true;
}

bool enc_placement(ByteWriter& w, const PlacementRecord& p) {
    if (!enc_g(w, p.id) || !enc_g(w, p.object) || !enc_g(w, p.object_generation) ||
        !enc_g(w, p.replica) || !enc_g(w, p.replica_generation) || !enc_g(w, p.backend) ||
        !enc_g(w, p.tier) || !enc_g(w, p.volume) || !enc_str(w, p.key) || !enc_g(w, p.manifest)) return false;
    w.put_u64(p.logical_size); w.put_u64(p.physical_size);
    if (!enc_digest(w, p.digest) || !w.put_u32(p.durability_replicas) || !enc_str(w, p.locality)) return false;
    if (!enc_enum(w, p.lifecycle) || !enc_enum(w, p.freshness) || !enc_g(w, p.authority_generation) ||
        !enc_enum(w, p.provenance) || !enc_g(w, p.writer_worker) || !enc_g(w, p.writer_boot) ||
        !enc_g(w, p.placement_generation)) return false;
    return enc_i64(w, p.created_at_ms) && enc_i64(w, p.available_at_ms);
}
bool dec_placement(ByteReader& r, PlacementRecord& p) {
    if (!dec_g(r, p.id) || !dec_g(r, p.object) || !dec_g(r, p.object_generation) ||
        !dec_g(r, p.replica) || !dec_g(r, p.replica_generation) || !dec_g(r, p.backend) ||
        !dec_g(r, p.tier) || !dec_g(r, p.volume) || !dec_str(r, p.key) || !dec_g(r, p.manifest)) return false;
    if (!r.read_u64(p.logical_size) || !r.read_u64(p.physical_size)) return false;
    if (!dec_digest(r, p.digest) || !r.read_u32(p.durability_replicas) || !dec_str(r, p.locality)) return false;
    std::uint8_t lc, fr, prov;
    if (!r.read_u8(lc)) return false; p.lifecycle = static_cast<PlacementLifecycle>(lc);
    if (!r.read_u8(fr)) return false; p.freshness = static_cast<Freshness>(fr);
    if (!dec_g(r, p.authority_generation)) return false;
    if (!r.read_u8(prov)) return false; p.provenance = static_cast<AuthorityOrigin>(prov);
    if (!dec_g(r, p.writer_worker) || !dec_g(r, p.writer_boot)) return false;
    if (!dec_g(r, p.placement_generation)) return false;
    if (!dec_i64(r, p.created_at_ms) || !dec_i64(r, p.available_at_ms)) return false;
    return true;
}

bool enc_replica_set(ByteWriter& w, const ReplicaSet& rs) {
    if (!enc_g(w, rs.object) || !enc_g(w, rs.current_generation)) return false;
    w.put_u32(rs.required); w.put_u32(rs.actual); w.put_u32(rs.distinct_failure_domains);
    w.put_u32(rs.authoritative_replicas);
    if (!enc_enum(w, rs.state) || !enc_g(w, rs.authority_generation)) return false;
    if (rs.replicas.size() > kMetaMaxRecords) return false;
    w.put_u32(static_cast<std::uint32_t>(rs.replicas.size()));
    for (const auto& rp : rs.replicas) {
        if (!enc_g(w, rp.id) || !enc_g(w, rp.generation) || !enc_g(w, rp.backend) || !enc_g(w, rp.node)) return false;
        if (!enc_str(w, rp.failure_domain)) return false;
        w.put_u64(rp.logical_size); w.put_u64(rp.physical_size);
        if (!enc_enum(w, rp.state) || !enc_g(w, rp.placement_generation) || !w.put_u8(rp.authoritative ? 1 : 0)) return false;
    }
    return enc_i64(w, rs.updated_at_ms);
}
bool dec_replica_set(ByteReader& r, ReplicaSet& rs) {
    if (!dec_g(r, rs.object) || !dec_g(r, rs.current_generation)) return false;
    if (!r.read_u32(rs.required) || !r.read_u32(rs.actual) || !r.read_u32(rs.distinct_failure_domains) ||
        !r.read_u32(rs.authoritative_replicas)) return false;
    if (!dec_enum<ReplicaState, 5>(r, rs.state) || !dec_g(r, rs.authority_generation)) return false;
    std::uint32_t n; if (!r.read_u32(n) || n > kMetaMaxRecords) return false;
    rs.replicas.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        ReplicaInfo rp;
        if (!dec_g(r, rp.id) || !dec_g(r, rp.generation) || !dec_g(r, rp.backend) || !dec_g(r, rp.node)) return false;
        if (!dec_str(r, rp.failure_domain)) return false;
        if (!r.read_u64(rp.logical_size) || !r.read_u64(rp.physical_size)) return false;
        if (!dec_enum<ReplicaState, 5>(r, rp.state) || !dec_g(r, rp.placement_generation)) return false;
        std::uint8_t auth; if (!r.read_u8(auth) || auth > 1) return false;
        rp.authoritative = auth == 1;
        rs.replicas.push_back(std::move(rp));
    }
    return dec_i64(r, rs.updated_at_ms);
}

bool enc_backend(ByteWriter& w, const BackendDescriptor& b) {
    if (!enc_g(w, b.id) || !enc_g(w, b.tier) || !enc_g(w, b.node) || !enc_str(w, b.name) || !enc_g(w, b.generation)) return false;
    w.put_u64(b.capacity.total_bytes); w.put_u64(b.capacity.free_bytes);
    w.put_u64(b.capacity.reserved_bytes); w.put_u64(b.capacity.committed_bytes);
    w.put_u64(b.capacity.reclaimable_bytes); w.put_u8(b.capacity.unknown ? 1 : 0);
    if (b.capabilities.size() > kMetaMaxRecords) return false;
    w.put_u32(static_cast<std::uint32_t>(b.capabilities.size()));
    for (const auto& cap : b.capabilities) { if (!enc_enum(w, cap)) return false; }
    return enc_enum(w, b.health) && enc_enum(w, b.freshness) && enc_enum(w, b.provenance) &&
           enc_i64(w, b.registered_at_ms);
}
bool dec_backend(ByteReader& r, BackendDescriptor& b) {
    if (!dec_g(r, b.id) || !dec_g(r, b.tier) || !dec_g(r, b.node) || !dec_str(r, b.name) || !dec_g(r, b.generation)) return false;
    if (!r.read_u64(b.capacity.total_bytes) || !r.read_u64(b.capacity.free_bytes) ||
        !r.read_u64(b.capacity.reserved_bytes) || !r.read_u64(b.capacity.committed_bytes) ||
        !r.read_u64(b.capacity.reclaimable_bytes)) return false;
    std::uint8_t unk; if (!r.read_u8(unk) || unk > 1) return false; b.capacity.unknown = unk == 1;
    std::uint32_t ncaps; if (!r.read_u32(ncaps) || ncaps > 16) return false;
    for (std::uint32_t i = 0; i < ncaps; ++i) {
        std::uint8_t cap; if (!r.read_u8(cap) || cap > 10) return false;
        b.capabilities.push_back(static_cast<BackendCapability>(cap));
    }
    return dec_enum<Health, 4>(r, b.health) && dec_enum<Freshness, 3>(r, b.freshness) &&
           dec_enum<MeasurementKind, 4>(r, b.provenance) && dec_i64(r, b.registered_at_ms);
}

bool enc_transfer(ByteWriter& w, const CompletedTransferRecord& t) {
    return enc_g(w, t.id) && enc_enum(w, t.kind) && enc_g(w, t.object) &&
           enc_g(w, t.source_backend) && enc_g(w, t.target_backend) &&
           w.put_u64(t.bytes) && enc_enum(w, t.state) &&
           enc_i64(w, t.finished_at_ms);
}
bool dec_transfer(ByteReader& r, CompletedTransferRecord& t) {
    if (!dec_g(r, t.id) || !dec_enum<MovementKind, 7>(r, t.kind) || !dec_g(r, t.object) ||
        !dec_g(r, t.source_backend) || !dec_g(r, t.target_backend)) return false;
    return r.read_u64(t.bytes) && dec_enum<TransferState, 6>(r, t.state) &&
           dec_i64(r, t.finished_at_ms);
}

bool enc_accounting(ByteWriter& w, const AccountingTotals& a) {
    w.put_u64(a.logical_objects); w.put_u64(a.logical_bytes); w.put_u64(a.physical_blobs);
    w.put_u64(a.physical_bytes); w.put_u64(a.deduplicated_bytes); w.put_u64(a.chunks);
    w.put_u64(a.manifests); w.put_u64(a.active_placements); w.put_u64(a.replicas);
    w.put_u64(a.reserved_bytes); w.put_u64(a.committed_bytes); w.put_u64(a.active_writes);
    w.put_u64(a.active_reads); w.put_u64(a.transferred_bytes); w.put_u64(a.restored_bytes);
    w.put_u64(a.evicted_bytes); w.put_u64(a.integrity_failures); w.put_u64(a.stale_rejections);
    w.put_u64(a.duplicate_rejections); w.put_u64(a.backend_failures); w.put_u64(a.participant_restarts);
    return true;
}
bool dec_accounting(ByteReader& r, AccountingTotals& a) {
    return r.read_u64(a.logical_objects) && r.read_u64(a.logical_bytes) &&
           r.read_u64(a.physical_blobs) && r.read_u64(a.physical_bytes) &&
           r.read_u64(a.deduplicated_bytes) && r.read_u64(a.chunks) &&
           r.read_u64(a.manifests) && r.read_u64(a.active_placements) &&
           r.read_u64(a.replicas) && r.read_u64(a.reserved_bytes) &&
           r.read_u64(a.committed_bytes) && r.read_u64(a.active_writes) &&
           r.read_u64(a.active_reads) && r.read_u64(a.transferred_bytes) &&
           r.read_u64(a.restored_bytes) && r.read_u64(a.evicted_bytes) &&
           r.read_u64(a.integrity_failures) && r.read_u64(a.stale_rejections) &&
           r.read_u64(a.duplicate_rejections) && r.read_u64(a.backend_failures) &&
           r.read_u64(a.participant_restarts);
}

}  // namespace

ContentDigest semantic_digest_of(ByteSpan serialized) {
    if (serialized.size() < 5) return ContentDigest();
    return ContentDigest::of(serialized.subspan(5));
}

PersistResult serialize_snapshot(const MetadataSnapshot& snap, Bytes& out) {
    PersistResult res;
    ByteWriter payload;
    if (!enc_enum(payload, static_cast<std::uint8_t>(kMetaVersion))) { res.code = StatusCode::Internal; return res; }
    if (!enc_g(payload, snap.epoch) || !enc_authority(payload, snap.authority)) { res.code = StatusCode::Internal; return res; }
    if (!enc_g(payload, snap.policy_generation)) { res.code = StatusCode::Internal; return res; }
    enc_i64(payload, snap.saved_at_ms);
    if (!enc_accounting(payload, snap.accounting)) { res.code = StatusCode::Internal; return res; }

    auto put_count = [](ByteWriter& w, std::uint32_t v) { w.put_u32(v); };
    auto write_vec = [&](auto& vec, auto&& enc) -> bool {
        if (vec.size() > kMetaMaxRecords) return false;
        put_count(payload, static_cast<std::uint32_t>(vec.size()));
        for (const auto& e : vec) { if (!enc(payload, e)) return false; }
        return true;
    };
    if (!write_vec(snap.objects, enc_object) || !write_vec(snap.manifests, enc_manifest) ||
        !write_vec(snap.placements, enc_placement) || !write_vec(snap.replica_sets, enc_replica_set) ||
        !write_vec(snap.backends, enc_backend) || !write_vec(snap.completed_transfers, enc_transfer) ||
        !write_vec(snap.authorities, enc_authority)) {
        res.code = StatusCode::Internal;
        return res;
    }

    Bytes pl = payload.take();
    const ContentDigest sem = ContentDigest::of(ByteSpan(pl.data(), pl.size()));
    const std::uint32_t crc = crc32(ByteSpan(pl.data(), pl.size()));

    ByteWriter w;
    w.put_u32(kMetaMagic);
    w.put_u8(kMetaVersion);
    w.put_u32(static_cast<std::uint32_t>(pl.size()));
    w.put_u32(crc);
    w.put_bytes(ByteSpan(sem.bytes.data(), 32));
    w.put_bytes(ByteSpan(pl.data(), pl.size()));
    out = w.take();

    res.ok = true;
    res.semantic_digest = sem;
    res.crc = crc;
    res.bytes = out.size();
    return res;
}

PersistResult deserialize_snapshot(ByteSpan data, MetadataSnapshot& snap) {
    PersistResult res;
    if (data.size() < 5) { res.code = StatusCode::Truncated; res.detail = "metadata blob too short"; return res; }
    ByteReader r(data);
    std::uint32_t magic = 0, payload_len = 0, stored_crc = 0;
    std::uint8_t ver = 0;
    if (!r.read_u32(magic) || !r.read_u8(ver) || !r.read_u32(payload_len) || !r.read_u32(stored_crc)) {
        res.code = StatusCode::Truncated; res.detail = "truncated header"; return res;
    }
    if (magic != kMetaMagic) { res.code = StatusCode::Malformed; res.detail = "bad metadata magic"; return res; }
    if (ver != kMetaVersion) { res.code = StatusCode::Malformed; res.detail = "unsupported metadata version"; return res; }
    if (payload_len > kMetaMaxRecords * 1024) { res.code = StatusCode::Overflow; res.detail = "metadata payload too large"; return res; }
    if (r.remaining() != static_cast<std::size_t>(payload_len) + 32) {
        res.code = StatusCode::TrailingGarbage; res.detail = "metadata size mismatch"; return res;
    }
    ByteSpan sem_bytes;
    if (!r.read_bytes(32, sem_bytes)) { res.code = StatusCode::Truncated; res.detail = "missing semantic digest"; return res; }
    ContentDigest stored_sem; std::memcpy(stored_sem.bytes.data(), sem_bytes.data(), 32);
    const ByteSpan payload = r.rest();
    const std::uint32_t recomputed_crc = crc32(payload);
    if (recomputed_crc != stored_crc) { res.code = StatusCode::Corrupted; res.detail = "metadata CRC mismatch"; return res; }
    const ContentDigest recomputed_sem = ContentDigest::of(payload);
    if (!(recomputed_sem == stored_sem)) { res.code = StatusCode::IntegrityMismatch; res.detail = "semantic digest mismatch"; return res; }

    ByteReader pr(payload);
    std::uint8_t pver; if (!pr.read_u8(pver) || pver != kMetaVersion) { res.code = StatusCode::Malformed; res.detail = "bad payload version"; return res; }
    if (!dec_g(pr, snap.epoch) || !dec_authority(pr, snap.authority)) { res.code = StatusCode::Malformed; res.detail = "bad authority"; return res; }
    if (!dec_g(pr, snap.policy_generation)) { res.code = StatusCode::Malformed; res.detail = "bad policy generation"; return res; }
    if (!dec_i64(pr, snap.saved_at_ms) || !dec_accounting(pr, snap.accounting)) { res.code = StatusCode::Malformed; res.detail = "bad accounting"; return res; }

    auto read_count = [&](std::uint32_t& n)->bool { return pr.read_u32(n) && n <= kMetaMaxRecords; };
    { std::uint32_t n; if (!read_count(n)) { res.code=StatusCode::Malformed; res.detail="objects count"; return res; }
      for (std::uint32_t i=0;i<n;++i){ snap.objects.emplace_back(); if(!dec_object(pr,snap.objects.back())){res.code=StatusCode::Malformed;res.detail="object";return res;} } }
    { std::uint32_t n; if (!read_count(n)) { res.code=StatusCode::Malformed; res.detail="manifests count"; return res; }
      for (std::uint32_t i=0;i<n;++i){ snap.manifests.emplace_back(); if(!dec_manifest(pr,snap.manifests.back())){res.code=StatusCode::Malformed;res.detail="manifest";return res;} } }
    { std::uint32_t n; if (!read_count(n)) { res.code=StatusCode::Malformed; res.detail="placements count"; return res; }
      for (std::uint32_t i=0;i<n;++i){ snap.placements.emplace_back(); if(!dec_placement(pr,snap.placements.back())){res.code=StatusCode::Malformed;res.detail="placement";return res;} } }
    { std::uint32_t n; if (!read_count(n)) { res.code=StatusCode::Malformed; res.detail="replica_sets count"; return res; }
      for (std::uint32_t i=0;i<n;++i){ snap.replica_sets.emplace_back(); if(!dec_replica_set(pr,snap.replica_sets.back())){res.code=StatusCode::Malformed;res.detail="replica set";return res;} } }
    { std::uint32_t n; if (!read_count(n)) { res.code=StatusCode::Malformed; res.detail="backends count"; return res; }
      for (std::uint32_t i=0;i<n;++i){ snap.backends.emplace_back(); if(!dec_backend(pr,snap.backends.back())){res.code=StatusCode::Malformed;res.detail="backend";return res;} } }
    { std::uint32_t n; if (!read_count(n)) { res.code=StatusCode::Malformed; res.detail="transfers count"; return res; }
      for (std::uint32_t i=0;i<n;++i){ snap.completed_transfers.emplace_back(); if(!dec_transfer(pr,snap.completed_transfers.back())){res.code=StatusCode::Malformed;res.detail="transfer";return res;} } }
    { std::uint32_t n; if (!read_count(n)) { res.code=StatusCode::Malformed; res.detail="authorities count"; return res; }
      for (std::uint32_t i=0;i<n;++i){ snap.authorities.emplace_back(); if(!dec_authority(pr,snap.authorities.back())){res.code=StatusCode::Malformed;res.detail="authority";return res;} } }

    if (!pr.empty()) { res.code = StatusCode::TrailingGarbage; res.detail = "trailing garbage after metadata"; return res; }

    std::unordered_set<std::uint64_t> obj_ids, placement_ids, manifest_ids, replica_ids;
    for (const auto& o : snap.objects) if (!obj_ids.insert(o.id.value()).second) { res.code=StatusCode::DuplicateIdentity; res.detail="duplicate object id"; return res; }
    for (const auto& m : snap.manifests) if (!manifest_ids.insert(m.id.value()).second) { res.code=StatusCode::DuplicateIdentity; res.detail="duplicate manifest id"; return res; }
    for (const auto& p : snap.placements) {
        if (!placement_ids.insert(p.id.value()).second) { res.code=StatusCode::DuplicateIdentity; res.detail="duplicate placement id"; return res; }
        if (!replica_ids.insert(p.replica.value()).second) { res.code=StatusCode::DuplicateIdentity; res.detail="duplicate replica id"; return res; }
    }

    res.ok = true;
    res.semantic_digest = stored_sem;
    res.crc = stored_crc;
    res.bytes = data.size();
    return res;
}

}  // namespace storagefabric
