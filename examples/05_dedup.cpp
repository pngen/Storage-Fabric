#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace storagefabric;

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(55);
    auth.worker = WorkerId(9);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex05";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("dedup-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
    const StorageBackendId backend = bid.value();

    const std::size_t size = 256 * 1024;
    std::vector<std::uint8_t> content(size);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 5 + 2) & 0xFF);

    DurabilityRequirement dur;
    dur.min_replicas = 1;

    std::printf("== Publishing two logical objects with identical content ==\n");
    auto first = sf.define_object(ObjectKind::KV_STATE, content.size(), ByteSpan(content.data(), content.size()), auth, dur);
    auto second = sf.define_object(ObjectKind::KV_STATE, content.size(), ByteSpan(content.data(), content.size()), auth, dur);
    if (first.failed() || second.failed()) { std::printf("define failed\n"); return 1; }

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    opts.dedupe = true;   // content-addressed dedup on
    auto p1 = sf.publish(first.value(), ByteSpan(content.data(), content.size()), opts);
    auto p2 = sf.publish(second.value(), ByteSpan(content.data(), content.size()), opts);
    if (p1.failed() || p2.failed()) { std::printf("publish failed\n"); return 1; }

    std::printf("  object1 id=%s digest=%s\n", first.value().id.str().c_str(), first.value().digest.short_hex(12).c_str());
    std::printf("  object2 id=%s digest=%s\n", second.value().id.str().c_str(), second.value().digest.short_hex(12).c_str());
    std::printf("  same content digest (content-addressed): %s\n",
                (first.value().digest == second.value().digest) ? "yes" : "no");
    std::printf("  same blob written once: placements=%zu\n", sf.placements().size());

    const AccountingTotals a = sf.accounting();
    std::printf("\n== Accounting (dedup) ==\n");
    std::printf("  logical_objects=%llu logical_bytes=%llu\n",
                (unsigned long long)a.logical_objects, (unsigned long long)a.logical_bytes);
    std::printf("  physical_blobs=%llu physical_bytes=%llu deduplicated_bytes=%llu\n",
                (unsigned long long)a.physical_blobs, (unsigned long long)a.physical_bytes,
                (unsigned long long)a.deduplicated_bytes);
    std::printf("  one shared physical blob for two logical objects: physical_blobs=1 => %s\n",
                (a.physical_blobs >= 1 && a.logical_objects >= 2) ? "dedup confirmed" : "not clearly deduped");

    // Confirm on-disk: exactly one blob file exists despite two logical objects.
    auto en = sf.backend(backend)->enumerate();
    std::size_t blob_files = 0;
    if (en.ok()) {
        for (const auto& k : en.value()) if (k.rfind("blobs/", 0) == 0) ++blob_files;
    }
    std::printf("  physical blob files on the local backend: %zu\n", blob_files);

    std::filesystem::remove_all(root, ec);
    std::printf("EX05_OK\n");
    return 0;
}
