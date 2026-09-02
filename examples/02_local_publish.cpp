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
    auth.boot = WorkerBootId(2026);
    auth.worker = WorkerId(7);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex02";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("local-example", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
    const StorageBackendId backend = bid.value();
    std::printf("registered local backend '%s' id=%s\n", sf.backend_descriptor(backend)->name.c_str(), backend.str().c_str());

    std::vector<std::uint8_t> content(64 * 1024);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>(i * 31 + 7);

    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::CHECKPOINT, content.size(),
                                    ByteSpan(content.data(), content.size()), auth, dur,
                                    RestorePriority::HIGH);
    if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    std::printf("== Object identity ==\n");
    std::printf("  id=%s generation=%s kind=%s\n", obj.id.str().c_str(), obj.generation.str().c_str(), to_string(obj.kind));
    std::printf("  digest=%s logical_size=%llu durability_min=%u restore=%s\n",
                obj.digest.short_hex(16).c_str(), (unsigned long long)obj.logical_size,
                obj.durability.min_replicas, to_string(obj.restore_priority));
    std::printf("  provenance origin=%s worker=%s boot=%s auth_gen=%s\n",
                to_string(obj.provenance.origin), obj.provenance.worker.str().c_str(),
                obj.provenance.boot.str().c_str(), obj.provenance.authority_generation.str().c_str());

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    opts.dedupe = true;
    opts.eager_verify = true;
    auto place_res = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
    if (place_res.failed()) { std::printf("publish failed: %s\n", place_res.error_message().c_str()); return 1; }
    const PlacementRecord p = place_res.value();

    std::printf("== Placement ==\n");
    std::printf("  id=%s backend=%s tier=%s lifecycle=%s\n", p.id.str().c_str(), p.backend.str().c_str(),
                p.tier.str().c_str(), to_string(p.lifecycle));
    std::printf("  digest=%s freshness=%s authority_gen=%s durability_replicas=%u\n",
                p.digest.short_hex(16).c_str(), to_string(p.freshness), p.authority_generation.str().c_str(),
                p.durability_replicas);

    auto rd = sf.read(obj.id);
    if (rd.failed()) { std::printf("read failed: %s\n", rd.error_message().c_str()); return 1; }
    const bool match = rd.value().size() == content.size() && std::memcmp(rd.value().data(), content.data(), content.size()) == 0;
    std::printf("== Read back ==\n");
    std::printf("  read bytes=%zu match=%d\n", rd.value().size(), match ? 1 : 0);

    auto vr = sf.verify(p.id);
    if (vr.ok()) {
        std::printf("  verify ok=1 code=%s size=%zu\n", status_name(vr.value().code), vr.value().size);
    } else {
        std::printf("  verify ok=0 code=%s err=%s\n", status_name(vr.error_code()), vr.error_message().c_str());
    }

    std::printf("== Backend & explanation ==\n");
    const BackendDescriptor* bd = sf.backend_descriptor(backend);
    std::printf("  backend name=%s provenance=%s health=%s freshness=%s\n",
                bd->name.c_str(), to_string(bd->provenance), to_string(bd->health), to_string(bd->freshness));
    std::printf("  %s\n", sf.explain_placement(obj.id).c_str());

    const AccountingTotals a = sf.accounting();
    std::printf("== Accounting ==\n");
    std::printf("  logical_objects=%llu logical_bytes=%llu physical_blobs=%llu physical_bytes=%llu active_placements=%llu\n",
                (unsigned long long)a.logical_objects, (unsigned long long)a.logical_bytes,
                (unsigned long long)a.physical_blobs, (unsigned long long)a.physical_bytes,
                (unsigned long long)a.active_placements);

    std::filesystem::remove_all(root, ec);
    std::printf("EX02_OK\n");
    return 0;
}
