#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/model/manifest.h"
#include "storagefabric/core/digest.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <algorithm>

using namespace storagefabric;

static ContentDigest compute_manifest_digest(const Manifest& m) {
    Sha256 h;
    auto push = [&](ByteSpan b) { h.update(b); };
    auto push_u64 = [&](std::uint64_t v) { std::uint8_t buf[8]; for (int i = 0; i < 8; ++i) buf[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF); push(ByteSpan(buf, 8)); };
    auto push_u32 = [&](std::uint32_t v) { std::uint8_t buf[4]; for (int i = 0; i < 4; ++i) buf[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF); push(ByteSpan(buf, 4)); };
    auto push_str = [&](const std::string& s) { push_u32(static_cast<std::uint32_t>(s.size())); push(ByteSpan(reinterpret_cast<const std::uint8_t*>(s.data()), s.size())); };
    push_str("SFB-MANIFEST-v1");
    push_u64(m.object.value());
    push_u64(m.object_generation.value());
    push_u64(m.total_logical_length);
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

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(44);
    auth.worker = WorkerId(6);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex04";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("chunk-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }

    const std::size_t total = 16 * 1024;
    const std::uint64_t chunk_size = 4096;
    std::vector<std::uint8_t> content(total);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 3 + 1) & 0xFF);

    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::CHECKPOINT, content.size(),
                                    ByteSpan(content.data(), content.size()), auth, dur);
    if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    std::printf("== Runtime publish with fixed chunk size ==\n");
    std::printf("  logical_size=%llu chunk_size=%llu -> expected chunk count=%llu\n",
                (unsigned long long)obj.logical_size, (unsigned long long)chunk_size,
                (unsigned long long)((obj.logical_size + chunk_size - 1) / chunk_size));
    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    opts.chunk_size = chunk_size;
    opts.eager_verify = true;
    auto place = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
    if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
    const PlacementRecord p = place.value();
    std::printf("  placement id=%s manifest=%s lifecycle=%s\n",
                p.id.str().c_str(), p.manifest.str().c_str(), to_string(p.lifecycle));

    auto read = sf.read(obj.id);
    if (read.failed()) { std::printf("read failed: %s\n", read.error_message().c_str()); return 1; }
    const bool match = read.value().size() == content.size() &&
                       std::memcmp(read.value().data(), content.data(), content.size()) == 0;
    std::printf("  multi-chunk reassembly: bytes=%zu match=%s\n", read.value().size(), match ? "yes" : "no");

    // Demonstrate the manifest model directly with the real chunking rules.
    std::printf("== Manifest model ==\n");
    Manifest m;
    m.id = ManifestId(1);
    m.generation = ManifestGeneration(1);
    m.object = obj.id;
    m.object_generation = obj.generation;
    m.total_logical_length = content.size();
    std::size_t offset = 0;
    std::uint64_t chunk_index = 0;
    while (offset < content.size()) {
        const std::size_t len = std::min<std::size_t>(static_cast<std::size_t>(chunk_size), content.size() - offset);
        ChunkDescriptor c;
        c.id = ChunkId(chunk_index + 1);
        c.generation = ChunkGeneration(1);
        c.offset = offset;
        c.logical_length = len;
        c.physical_length = len;
        c.digest = ContentDigest::of(ByteSpan(content.data() + offset, len));
        c.blob = BlobId(chunk_index + 1);
        c.provenance = AuthorityOrigin::WORKER;
        m.chunks.push_back(c);
        std::printf("  chunk%llu offset=%llu agg_len=%llu digest=%s\n",
                    (unsigned long long)c.id.value(), (unsigned long long)c.offset,
                    (unsigned long long)c.logical_length, c.digest.short_hex(10).c_str());
        offset += len;
        ++chunk_index;
    }
    m.sort_chunks();
    m.manifest_digest = compute_manifest_digest(m);
    const Status mv = m.validate();
    std::printf("  manifest.validate() ok=%s code=%s\n", mv.ok() ? "yes" : "no",
                status_name(mv.code()));
    const ManifestValidation vv = validate_manifest(m);
    std::printf("  validate_manifest() ok=%s detail=%s\n", vv.ok ? "yes" : "no", vv.detail.c_str());
    std::printf("  manifest.objects=%s chunks=%zu total=%llu\n",
                m.object.str().c_str(), m.chunks.size(),
                (unsigned long long)m.total_logical_length);

    // A manifest with a gap must be rejected.
    Manifest gap = m;
    gap.chunks.back().offset += 1;   // create a gap after sorting
    gap.sort_chunks();
    const ManifestValidation gv = validate_manifest(gap);
    std::printf("  gap manifest validate_manifest() ok=%s (expected no)\n", gv.ok ? "yes" : "no");

    std::filesystem::remove_all(root, ec);
    std::printf("EX04_OK\n");
    return 0;
}
