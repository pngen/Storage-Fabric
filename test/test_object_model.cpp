#include "test_util.h"
#include "storagefabric/model/object.h"
#include "storagefabric/model/enums.h"

#include <initializer_list>

using namespace storagefabric;

int main() {
    std::printf("test_object_model starting\n");

    // ---- kind parse / to_string round-trip ----
    const std::initializer_list<ObjectKind> kinds = {
        ObjectKind::MODEL_SHARD, ObjectKind::CHECKPOINT, ObjectKind::TENSOR,
        ObjectKind::KV_STATE, ObjectKind::PREFIX_STATE, ObjectKind::COMPILED_ARTIFACT,
        ObjectKind::EXECUTION_GRAPH, ObjectKind::EXECUTION_PLAN, ObjectKind::ADAPTER,
        ObjectKind::DATASET_SHARD, ObjectKind::LOGICAL_BLOB, ObjectKind::GENERIC_AI_STATE};
    for (const ObjectKind k : kinds) {
        const char* s = to_string(k);
        CHECK(s != nullptr);
        const auto parsed = parse_object_kind(s);
        CHECK(parsed.has_value());
        CHECK_EQ(static_cast<int>(parsed.value()), static_cast<int>(k));
    }
    // Case-insensitive parsing.
    CHECK(parse_object_kind("checkpoint").has_value());
    CHECK(parse_object_kind("Checkpoint").has_value());
    CHECK(parse_object_kind("CHECKPOINT").has_value());
    CHECK(!parse_object_kind("not-a-kind").has_value());
    CHECK(!parse_object_kind("").has_value());
    std::printf("  kind parse/to_string round-trip PASS\n");

    // ---- durability ----
    DurabilityRequirement dur;
    dur.min_replicas = 2;
    CHECK(dur.is_satisfied_by(2));
    CHECK(dur.is_satisfied_by(3));
    CHECK(!dur.is_satisfied_by(1));
    CHECK(!dur.is_satisfied_by(0));
    DurabilityRequirement zero_dur;
    zero_dur.min_replicas = 0;   // treated as optional/ephemeral
    CHECK(zero_dur.is_satisfied_by(0));   // durability 0 is satisfied by 0 replicas
    std::printf("  durability PASS\n");

    // ---- ObjectDescriptor fields ----
    ObjectDescriptor obj;
    obj.id = ObjectId(42);
    obj.generation = ObjectGeneration(3);
    obj.kind = ObjectKind::CHECKPOINT;
    obj.logical_size = 4096;
    const Bytes content = {'h', 'e', 'l', 'l', 'o'};
    obj.digest = ContentDigest::of(ByteSpan(content.data(), content.size()));
    obj.owner = OwnerId(9);
    obj.provenance.origin = AuthorityOrigin::RUNNER;
    obj.provenance.worker = WorkerId(5);
    obj.provenance.boot = WorkerBootId(7);
    obj.provenance.authority_generation = AuthorityGeneration(2);
    obj.provenance.creator = "runner:producer";
    obj.durability.min_replicas = 2;
    obj.restore_priority = RestorePriority::HIGH;
    obj.locality.preferred = StorageClass::LOCAL_NVME;
    obj.locality.node_hint = "node-7";
    obj.retention = RetentionPolicy::PROTECTED;
    obj.policy_generation = PolicyGeneration(4);
    obj.dependencies = {ObjectId(1), ObjectId(2)};
    obj.compatibility["format"] = "v2";
    obj.compatibility["checksum"] = "xxhash";
    obj.created_at_ms = 123456;

    CHECK(obj.is_well_formed());
    CHECK(obj.has_valid_digest());
    CHECK_EQ(static_cast<int>(obj.kind), static_cast<int>(ObjectKind::CHECKPOINT));
    CHECK_EQ(static_cast<int>(obj.restore_priority), static_cast<int>(RestorePriority::HIGH));
    CHECK_EQ(static_cast<int>(obj.locality.preferred), static_cast<int>(StorageClass::LOCAL_NVME));
    CHECK_EQ(static_cast<int>(obj.retention), static_cast<int>(RetentionPolicy::PROTECTED));
    CHECK_EQ(obj.logical_size, 4096u);
    CHECK_EQ(obj.owner.value(), 9u);
    CHECK(obj.dependencies.size() == 2u);
    CHECK(obj.compatibility.at("format") == "v2");
    CHECK(obj.compatibility.at("checksum") == "xxhash");
    std::printf("  ObjectDescriptor fields PASS\n");

    // ---- normalization ----
    ObjectDescriptor norm;
    norm.id = ObjectId(1);
    norm.durability.min_replicas = 0;   // must be normalized up to 1
    norm.restore_priority = RestorePriority::NORMAL;
    CHECK(norm.durability.min_replicas == 0u);
    norm.normalize();
    CHECK(norm.durability.min_replicas == 1u);
    std::printf("  normalization PASS\n");

    // Digest determinism: same content -> same digest.
    ObjectDescriptor a, b;
    a.digest = ContentDigest::of(ByteSpan(content.data(), content.size()));
    b.digest = ContentDigest::of(ByteSpan(content.data(), content.size()));
    CHECK(a.digest == b.digest);

    std::printf("test_object_model: ALL PASS\n");
    return 0;
}
