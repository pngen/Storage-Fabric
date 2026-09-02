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
    auth.boot = WorkerBootId(66);
    auth.worker = WorkerId(12);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path rootA = std::filesystem::temp_directory_path() / "sfb-ex06-a";
    const std::filesystem::path rootB = std::filesystem::temp_directory_path() / "sfb-ex06-b";
    std::error_code ec;
    std::filesystem::remove_all(rootA, ec);
    std::filesystem::remove_all(rootB, ec);
    auto bidA = sf.register_local_backend("replica-A", rootA, StorageClass::LOCAL_FILESYSTEM);
    auto bidB = sf.register_local_backend("replica-B", rootB, StorageClass::LOCAL_FILESYSTEM);
    if (bidA.failed() || bidB.failed()) { std::printf("register failed\n"); return 1; }
    const StorageBackendId backendA = bidA.value();
    const StorageBackendId backendB = bidB.value();

    std::vector<std::uint8_t> content(48 * 1024);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 11 + 7) & 0xFF);

    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::ADAPTER, content.size(),
                                    ByteSpan(content.data(), content.size()), auth, dur);
    if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    auto p1 = sf.publish(obj, ByteSpan(content.data(), content.size()), opts);
    if (p1.failed()) { std::printf("publish failed: %s\n", p1.error_message().c_str()); return 1; }
    const PlacementRecord first = p1.value();

    std::printf("== Replica set after initial publish ==\n");
    auto rs1 = sf.replica_sets();
    for (const auto& rs : rs1) {
        std::printf("  object=%s required=%u actual=%u authoritative=%u distinct_domains=%u state=%s\n",
                    rs.object.str().c_str(), rs.required, rs.actual, rs.authoritative_replicas,
                    rs.distinct_failure_domains, to_string(rs.state));
        for (const auto& ri : rs.replicas) {
            std::printf("    replica id=%s backend=%s node=%s domain=%s state=%s auth=%s\n",
                        ri.id.str().c_str(), ri.backend.str().c_str(), ri.node.str().c_str(),
                        ri.failure_domain.c_str(), to_string(ri.state), ri.authoritative ? "yes" : "no");
        }
    }
    std::printf("  %s\n", sf.explain_replication(obj.id).c_str());

    // Replicate the authoritative placement to backend B.
    // Cross-backend copy: disable content-dedup so the target writes its OWN blob
    // (blobs are per-backend; the source blob lives only on backend A).
    PublishOptions cross = opts;
    cross.dedupe = false;
    auto p2 = sf.replicate(first, backendB, cross);
    if (p2.failed()) { std::printf("replicate failed: %s\n", p2.error_message().c_str()); return 1; }
    const PlacementRecord second = p2.value();

    std::printf("\n== After replicate to backend B ==\n");
    std::printf("  source placement id=%s backend=%s lifecycle=%s\n",
                first.id.str().c_str(), first.backend.str().c_str(), to_string(first.lifecycle));
    std::printf("  replicated placement id=%s backend=%s lifecycle=%s\n",
                second.id.str().c_str(), second.backend.str().c_str(), to_string(second.lifecycle));
    std::printf("  total governed placements for object=%s: %zu\n",
                obj.id.str().c_str(), sf.placements().size());
    auto rs2 = sf.replica_sets();
    for (const auto& rs : rs2) {
        std::printf("  replica_set: required=%u actual=%u authoritative=%u domains=%u state=%s\n",
                    rs.required, rs.actual, rs.authoritative_replicas, rs.distinct_failure_domains, to_string(rs.state));
    }
    std::printf("  %s\n", sf.explain_replication(obj.id).c_str());

    // Read back through the replicated placement by id.
    auto read = sf.read(obj.id, second.id);
    const bool match = read.ok() && read.value().size() == content.size() &&
                       std::memcmp(read.value().data(), content.data(), content.size()) == 0;
    std::printf("\n  read through replicated placement %s: bytes=%zu match=%s\n",
                second.id.str().c_str(), read.ok() ? read.value().size() : 0, match ? "yes" : "no");

    std::filesystem::remove_all(rootA, ec);
    std::filesystem::remove_all(rootB, ec);
    std::printf("EX06_OK\n");
    return 0;
}
