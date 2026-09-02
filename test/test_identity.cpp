#include "test_util.h"
#include "storagefabric/core/strong.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/model/authority.h"

#include <type_traits>
#include <unordered_map>
#include <set>

using namespace storagefabric;

// Strong types are non-interchangeable: distinct identity domains cannot be
// compared or converted across each other.
static_assert(!std::is_same_v<ObjectId, BlobId>, "ObjectId and BlobId must differ");
static_assert(!std::is_same_v<ObjectId, PlacementId>, "ObjectId and PlacementId must differ");
static_assert(!std::is_same_v<PlacementId, ReplicaId>, "PlacementId and ReplicaId must differ");
static_assert(!std::is_same_v<ObjectGeneration, CoordinatorEpoch>, "generation tags must differ");
static_assert(!std::is_convertible_v<ObjectId, BlobId>, "ObjectId must not convert to BlobId");

int main() {
    std::printf("test_identity starting\n");

    // ---- strong value semantics ----
    ObjectId o1(5);
    ObjectId o2(5);
    ObjectId o3(6);
    CHECK(o1 == o2);
    CHECK(!(o1 == o3));
    CHECK(o1.value() == 5u);
    CHECK(!o1.is_nil());
    ObjectId nil;
    CHECK(nil.is_nil());
    CHECK(o1.next().value() == 6u);
    CHECK((o3 > o1));

    // ---- generation ordering ----
    ObjectGeneration g1(1);
    ObjectGeneration g2(2);
    CHECK(g2.exceeds(g1));
    CHECK(g1.precedes(g2));
    CHECK(g2.is_fresh_after(g1));
    CHECK(g1.coincident_with(ObjectGeneration(1)));
    CHECK(!(g1 == g2));
    CHECK(g1.next().value() == 2u);
    CHECK(g2 > g1);
    std::printf("  generation ordering PASS\n");

    // A bare generation only means something within an authority context; the
    // authority envelope decides cross-context freshness.
    CoordinatorEpoch e1(1);
    CoordinatorEpoch e3(3);
    CHECK(e3.exceeds(e1));

    // ---- authority envelope lexicographic compare ----
    // Stale boot with a very high local generation must NOT fence a fresh
    // process incarnation: epoch dominates, then boot, then generation.
    AuthorityEnvelope stale_boot_high_gen;
    stale_boot_high_gen.epoch = CoordinatorEpoch(1);
    stale_boot_high_gen.boot = WorkerBootId(100);
    stale_boot_high_gen.worker = WorkerId(7);
    stale_boot_high_gen.generation = AuthorityGeneration(9999);
    stale_boot_high_gen.origin = AuthorityOrigin::WORKER;

    AuthorityEnvelope fresh_epoch_low_gen;
    fresh_epoch_low_gen.epoch = CoordinatorEpoch(2);
    fresh_epoch_low_gen.boot = WorkerBootId(1);
    fresh_epoch_low_gen.worker = WorkerId(7);
    fresh_epoch_low_gen.generation = AuthorityGeneration(1);
    fresh_epoch_low_gen.origin = AuthorityOrigin::COORDINATOR;

    CHECK(!stale_boot_high_gen.is_strictly_newer_than(fresh_epoch_low_gen));
    CHECK(fresh_epoch_low_gen.is_strictly_newer_than(stale_boot_high_gen));
    CHECK(AuthorityEnvelope::compare(stale_boot_high_gen, fresh_epoch_low_gen) < 0);
    CHECK(AuthorityEnvelope::compare(fresh_epoch_low_gen, stale_boot_high_gen) > 0);
    CHECK(is_authoritative_after(fresh_epoch_low_gen, stale_boot_high_gen));
    CHECK(!is_authoritative_after(stale_boot_high_gen, fresh_epoch_low_gen));
    std::printf("  stale boot never fences a fresh epoch PASS\n");

    // Same epoch+boot: higher generation wins (within context).
    AuthorityEnvelope a;
    a.epoch = CoordinatorEpoch(5);
    a.boot = WorkerBootId(9);
    a.worker = WorkerId(2);
    a.generation = AuthorityGeneration(3);
    AuthorityEnvelope b = a;
    b.generation = AuthorityGeneration(40);
    CHECK(b.is_strictly_newer_than(a));
    CHECK(!a.is_strictly_newer_than(b));

    // Same epoch, different boot: higher boot wins regardless of its generation.
    AuthorityEnvelope c = a;
    c.boot = WorkerBootId(50);
    c.generation = AuthorityGeneration(1);  // low gen but newer boot
    CHECK(c.is_strictly_newer_than(a));
    CHECK(!a.is_strictly_newer_than(c));
    std::printf("  authority lexicographic compare PASS\n");

    // ---- hash-ability ----
    std::unordered_map<ObjectId, int> id_map;
    id_map.emplace(ObjectId(1), 10);
    id_map.emplace(ObjectId(2), 20);
    id_map.emplace(ObjectId(3), 30);
    CHECK(id_map[ObjectId(1)] == 10);
    CHECK(id_map[ObjectId(2)] == 20);
    CHECK(id_map[ObjectId(3)] == 30);
    CHECK(id_map.size() == 3);

    std::unordered_map<ObjectGeneration, int> gen_map;
    gen_map.emplace(ObjectGeneration(1), 100);
    gen_map.emplace(ObjectGeneration(2), 200);
    CHECK(gen_map[ObjectGeneration(2)] == 200);
    CHECK(gen_map.count(ObjectGeneration(3)) == 0);

    std::unordered_map<ContentDigest, int> digest_map;
    const Bytes data = {'a', 'b', 'c'};
    const ContentDigest d = ContentDigest::of(ByteSpan(data.data(), data.size()));
    digest_map.emplace(d, 7);
    CHECK(digest_map[ContentDigest::of(ByteSpan(data.data(), data.size()))] == 7);
    std::printf("  hash-ability PASS\n");

    // Distinct identity domains can coexist in a set without collision.
    std::set<ObjectId> obj_set = {ObjectId(1), ObjectId(1), ObjectId(2)};
    CHECK(obj_set.size() == 2);

    std::printf("test_identity: ALL PASS\n");
    return 0;
}
