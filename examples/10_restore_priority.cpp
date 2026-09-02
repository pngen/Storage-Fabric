#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace storagefabric;

static std::vector<std::uint8_t> make_content(std::size_t n, std::uint64_t seed) {
    std::vector<std::uint8_t> v(n);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>((i * 29 + seed) & 0xFF);
    return v;
}

int main() {
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(99);
    auth.worker = WorkerId(21);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex10";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("restore-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }

    struct Item {
        const char* label;
        ObjectKind kind;
        RestorePriority prio;
        std::uint64_t seed;
    };
    const Item items[] = {
        {"critical", ObjectKind::EXECUTION_PLAN, RestorePriority::CRITICAL, 1},
        {"high", ObjectKind::EXECUTION_GRAPH, RestorePriority::HIGH, 2},
        {"normal", ObjectKind::TENSOR, RestorePriority::NORMAL, 3},
        {"background", ObjectKind::DATASET_SHARD, RestorePriority::BACKGROUND, 4},
    };

    DurabilityRequirement dur;
    dur.min_replicas = 1;
    std::printf("== Restore priority is a first-class object property ==\n");
    for (const Item& it : items) {
        std::vector<std::uint8_t> content = make_content(16 * 1024, it.seed);
        auto obj_res = sf.define_object(it.kind, content.size(), ByteSpan(content.data(), content.size()),
                                        auth, dur, it.prio);
        if (obj_res.failed()) { std::printf("define %s failed\n", it.label); return 1; }
        const ObjectDescriptor obj = obj_res.value();
        PublishOptions opts;
        opts.authority = auth;
        opts.required_replicas = 1;
        auto pl = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
        if (pl.failed()) { std::printf("publish %s failed: %s\n", it.label, pl.error_message().c_str()); return 1; }
        std::printf("  %-11s object=%s kind=%s priority=%-10s durability_min=%u placement=%s\n",
                    it.label, obj.id.str().c_str(), to_string(obj.kind), to_string(obj.restore_priority),
                    obj.durability.min_replicas, pl.value().id.str().c_str());
        std::printf("      explain_restore: %s\n", sf.explain_restore(obj.id).c_str());
    }

    // Restore priority is retained on the object descriptor and drives the restore path.
    std::printf("\n== Restore ordering clue (priority retained on the descriptor) ==\n");
    const auto objects = sf.objects();
    for (const ObjectDescriptor& o : objects) {
        std::printf("  object=%s kind=%s priority=%s durability_min=%u\n",
                    o.id.str().c_str(), to_string(o.kind), to_string(o.restore_priority), o.durability.min_replicas);
    }

    // A restore reads from the authoritative replica first (per the explanation API).
    std::printf("\n== Restore reads from the authoritative copy ==\n");
    for (const ObjectDescriptor& o : objects) {
        auto rd = sf.read(o.id);
        std::printf("  object=%s read_back=%s bytes=%zu\n",
                    o.id.str().c_str(), rd.ok() ? "ok" : "failed", rd.ok() ? rd.value().size() : 0);
    }
    std::printf("  %s\n", sf.explain_restore(objects.front().id).c_str());

    std::filesystem::remove_all(root, ec);
    std::printf("EX10_OK\n");
    return 0;
}
