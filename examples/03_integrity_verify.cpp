#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace storagefabric;

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(33);
    auth.worker = WorkerId(5);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex03";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("integrity-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
    const StorageBackendId backend = bid.value();

    std::vector<std::uint8_t> content(32 * 1024);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 13 + 9) & 0xFF);

    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::MODEL_SHARD, content.size(),
                                    ByteSpan(content.data(), content.size()), auth, dur);
    if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    opts.eager_verify = true;   // verifies each chunk digest right after write
    auto place = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
    if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
    const PlacementRecord p = place.value();

    std::printf("== Baseline integrity ==\n");
    auto v0 = sf.verify(p.id);
    std::printf("  verify on clean copy: ok=%s code=%s size=%zu\n",
                v0.ok() ? "yes" : "no", status_name(v0.ok() ? v0.value().code : v0.error_code()),
                v0.ok() ? v0.value().size : 0);

    // Find the physical blob file on disk and corrupt a byte in the middle.
    std::string blobkey;
    auto en = sf.backend(backend)->enumerate();
    if (en.ok()) {
        for (const auto& k : en.value()) {
            if (k.rfind("blobs/", 0) == 0) { blobkey = k; break; }
        }
    }
    if (!blobkey.empty()) {
        const std::filesystem::path blobfile = root / "objects" / blobkey;
        std::fstream f(blobfile, std::ios::in | std::ios::out | std::ios::binary);
        if (f.is_open()) {
            f.seekp(static_cast<std::streamoff>(content.size() / 2));
            f.put(static_cast<char>(0x00));   // corrupt one byte
            f.flush();
            f.close();
            std::printf("\n  corrupted physical blob '%s' at offset %zu\n",
                        blobkey.c_str(), content.size() / 2);
        }
    } else {
        std::printf("\n  could not locate a blob file to corrupt\n");
    }

    std::printf("== Integrity after on-disk corruption ==\n");
    auto v1 = sf.verify(p.id);
    if (v1.ok()) {
        std::printf("  verify ok=%s code=%s size=%zu\n", "yes", status_name(v1.value().code), v1.value().size);
    } else {
        std::printf("  verify ok=no code=%s (%s)\n", status_name(v1.error_code()), v1.error_message().c_str());
    }
    const auto raw = sf.read(obj.id);
    std::printf("  read still returns bytes (reassembly is unchecked): len=%s\n",
                raw.ok() ? std::to_string(raw.value().size()).c_str() : "n/a");
    std::printf("  accounting.integrity_failures=%llu (detected by verify)\n",
                (unsigned long long)sf.accounting().integrity_failures);
    std::printf("  %s\n", sf.explain_failure(obj.id).c_str());

    std::filesystem::remove_all(root, ec);
    std::printf("EX03_OK\n");
    return 0;
}
