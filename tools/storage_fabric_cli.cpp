// Storage Fabric - single-file "storage-fabric" CLI.
// Demonstrates the StorageFabric public API end-to-end (16 subcommands). Every
// subcommand builds a fresh runtime and prints the provenance/freshness/backend/
// tier/generation/digest/durability/authority fields the API exposes. Only the
// public include/ API is used. Build: --target storage_fabric_cli

#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/storage/synthetic_backend.h"
#include "storagefabric/core/planner.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/bytes.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>

using namespace storagefabric;

namespace {

// A deterministic writer authority used by the CLI's single "session writer".
AuthorityEnvelope cli_authority() {
    AuthorityEnvelope a;
    a.epoch = CoordinatorEpoch(1);
    a.boot = WorkerBootId(0xCAFE);
    a.worker = WorkerId(0xDEC0);
    a.generation = AuthorityGeneration(1);
    a.origin = AuthorityOrigin::RUNNER;
    return a;
}

// ---- argument helpers ------------------------------------------------------

bool parse_u64(const std::string& s, std::uint64_t& out) {
    const char* b = s.data();
    const char* e = b + s.size();
    const auto res = std::from_chars(b, e, out);
    return res.ec == std::errc() && res.ptr == e;
}

bool parse_u32(const std::string& s, std::uint32_t& out) {
    std::uint64_t v = 0;
    if (!parse_u64(s, v)) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

std::optional<ObjectKind> parse_kind(const std::string& s) {
    if (s.empty()) return ObjectKind::GENERIC_AI_STATE;
    return parse_object_kind(s);
}

// Builds deterministic content of exactly 'size' bytes. <text> is placed first
// and the remainder is filled with a ramp so that size is always honored.
Bytes build_content(std::uint64_t size, const std::string& text) {
    Bytes out(static_cast<std::size_t>(size), 0);
    const std::size_t n = static_cast<std::size_t>(size);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    }
    const std::size_t tlen = text.size() < n ? text.size() : n;
    for (std::size_t i = 0; i < tlen; ++i) {
        out[i] = static_cast<std::uint8_t>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

std::string backend_label(const StorageFabric& sf, StorageBackendId id) {
    const BackendDescriptor* bd = sf.backend_descriptor(id);
    return bd ? bd->name : ("backend-" + id.str());
}

// ---- printing primitives ---------------------------------------------------

void print_object(const ObjectDescriptor& o) {
    std::printf("  object: id=%s generation=%s kind=%s logical_size=%llu\n",
                o.id.str().c_str(), o.generation.str().c_str(), to_string(o.kind),
                static_cast<unsigned long long>(o.logical_size));
    std::printf("    digest=%s owner=%s\n", o.digest.hex().c_str(), o.owner.str().c_str());
    std::printf("    durability.min_replicas=%u restore_priority=%s retention=%s\n",
                o.durability.min_replicas, to_string(o.restore_priority), to_string(o.retention));
    std::printf("    provenance.origin=%s worker=%s boot=%s authority_generation=%s\n",
                to_string(o.provenance.origin), o.provenance.worker.str().c_str(),
                o.provenance.boot.str().c_str(), o.provenance.authority_generation.str().c_str());
    std::printf("    policy_generation=%s\n", o.policy_generation.str().c_str());
}

void print_placement(const PlacementRecord& p, const StorageFabric& sf) {
    const char* tier_name = "?";
    const StorageTier* t = sf.tier(p.tier);
    if (t) tier_name = t->name.c_str();
    std::printf("  placement: id=%s object=%s backend=%s(%s) tier=%s(%s)\n",
                p.id.str().c_str(), p.object.str().c_str(), p.backend.str().c_str(),
                backend_label(sf, p.backend).c_str(), p.tier.str().c_str(), tier_name);
    std::printf("    object_generation=%s replica=%s replica_generation=%s placement_generation=%s\n",
                p.object_generation.str().c_str(), p.replica.str().c_str(),
                p.replica_generation.str().c_str(), p.placement_generation.str().c_str());
    std::printf("    lifecycle=%s freshness=%s durability_replicas=%u\n",
                to_string(p.lifecycle), to_string(p.freshness), p.durability_replicas);
    std::printf("    logical_size=%llu physical_size=%llu digest=%s\n",
                static_cast<unsigned long long>(p.logical_size),
                static_cast<unsigned long long>(p.physical_size), p.digest.hex().c_str());
    std::printf("    authority_generation=%s provenance=%s writer_worker=%s writer_boot=%s\n",
                p.authority_generation.str().c_str(), to_string(p.provenance),
                p.writer_worker.str().c_str(), p.writer_boot.str().c_str());
    std::printf("    key=%s manifest=%s volume=%s\n", p.key.c_str(), p.manifest.str().c_str(),
                p.volume.str().c_str());
}

void print_backend(const StorageFabric& sf, StorageBackendId id) {
    const BackendDescriptor* bd = sf.backend_descriptor(id);
    if (!bd) { std::printf("  backend %s: <unknown>\n", id.str().c_str()); return; }
    const StorageTier* t = sf.tier(bd->tier);
    const char* cls = t ? to_string(t->storage_class) : "UNKNOWN";
    std::printf("  backend: id=%s name=%s tier=%s class=%s\n",
                bd->id.str().c_str(), bd->name.c_str(), bd->tier.str().c_str(), cls);
    std::printf("    health=%s freshness=%s provenance=%s generation=%s\n",
                to_string(bd->health), to_string(bd->freshness), to_string(bd->provenance),
                bd->generation.str().c_str());
    if (t) {
        std::printf("    tier_capacity.total=%llu free=%llu durability_class=%s evictable=%d persistent=%d\n",
                    static_cast<unsigned long long>(bd->capacity.total_bytes),
                    static_cast<unsigned long long>(bd->capacity.free_bytes),
                    to_string(t->durability_class), t->eviction_capable ? 1 : 0, t->persistent ? 1 : 0);
    }
}

void print_plan(const StoragePlan& sp, const StorageFabric& sf) {
    std::printf("  plan: feasible=%d object=%s generation=%s\n",
                sp.feasible ? 1 : 0, sp.object.str().c_str(), sp.object_generation.str().c_str());
    for (std::size_t i = 0; i < sp.ranked.size(); ++i) {
        const PlacementCandidate& c = sp.ranked[i];
        std::printf("  candidate[%zu]: backend=%s(%s) viable=%d score=%.4f\n",
                    i, c.backend.str().c_str(), backend_label(sf, c.backend).c_str(),
                    c.viable ? 1 : 0, c.score);
        for (const auto& f : c.factors) {
            std::printf("      factor name=%s value=%.4f weight=%.4f kind=%s evidence=%s\n",
                        f.name.c_str(), f.value, f.weight, to_string(f.kind), f.evidence.c_str());
        }
        for (const auto& v : c.hard_violations) std::printf("      hard_violation=%s\n", v.c_str());
    }
    if (sp.selected) {
        std::printf("  selected: backend=%s(%s) score=%.4f\n", sp.selected->backend.str().c_str(),
                    backend_label(sf, sp.selected->backend).c_str(), sp.selected->score);
    } else {
        std::printf("  selected: <none>\n");
    }
}

void print_accounting(const AccountingTotals& a) {
    std::printf("  accounting: objects=%llu logical_bytes=%llu blobs=%llu physical_bytes=%llu\n",
                static_cast<unsigned long long>(a.logical_objects),
                static_cast<unsigned long long>(a.logical_bytes),
                static_cast<unsigned long long>(a.physical_blobs),
                static_cast<unsigned long long>(a.physical_bytes));
    std::printf("    active_placements=%llu replicas=%llu reserved=%llu committed=%llu\n",
                static_cast<unsigned long long>(a.active_placements),
                static_cast<unsigned long long>(a.replicas),
                static_cast<unsigned long long>(a.reserved_bytes),
                static_cast<unsigned long long>(a.committed_bytes));
    std::printf("    stale_rejections=%llu integrity_failures=%llu backend_failures=%llu\n",
                static_cast<unsigned long long>(a.stale_rejections),
                static_cast<unsigned long long>(a.integrity_failures),
                static_cast<unsigned long long>(a.backend_failures));
}

// Registers a real local backend under a unique temp dir. 'tok' keeps multiple
// backends on the same runtime from sharing a root.
Result<StorageBackendId> register_local(StorageFabric& sf, const std::string& name, const std::string& tok) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("storagefabric-cli-" + tok);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return sf.register_local_backend(name, root, StorageClass::LOCAL_FILESYSTEM);
}

// Creates + publishes a demonstration object (to 'backend'), then reads and
// verifies it, so every subcommand stays self-contained.
int demo_roundtrip(StorageFabric& sf, StorageBackendId backend, const std::string& kind,
                   std::uint64_t size, const std::string& text, bool verbose) {
    const auto kind_opt = parse_kind(kind);
    if (!kind_opt) { std::fprintf(stderr, "unknown object kind: %s\n", kind.c_str()); return 2; }
    const AuthorityEnvelope auth = sf.authority();
    DurabilityRequirement dur; dur.min_replicas = 1;
    Bytes content = build_content(size, text);

    auto obj_res = sf.define_object(*kind_opt, size, ByteSpan(content.data(), content.size()),
                                    auth, dur, RestorePriority::HIGH);
    if (obj_res.failed()) { std::printf("define_object failed: %s\n", obj_res.error_message().c_str()); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    opts.dedupe = true;
    opts.eager_verify = true;
    auto place_res = sf.publish_to(obj, ByteSpan(content.data(), content.size()), backend, opts);
    if (place_res.failed()) { std::printf("publish_to failed: %s\n", place_res.error_message().c_str()); return 1; }
    const PlacementRecord p = place_res.value();

    if (verbose) {
        std::printf("== defined object ==\n"); print_object(obj);
        std::printf("== published placement ==\n"); print_placement(p, sf);
    }

    auto rd = sf.read(obj.id);
    if (rd.failed()) { std::printf("read failed: %s\n", rd.error_message().c_str()); return 1; }
    const bool match = rd.value().size() == content.size() &&
                       std::memcmp(rd.value().data(), content.data(), content.size()) == 0;
    auto vr = sf.verify(p.id);
    std::printf("== read back ==\n  read_bytes=%zu content_match=%d\n", rd.value().size(), match ? 1 : 0);
    if (vr.ok()) {
        std::printf("  verify: ok=%d code=%s size=%zu\n", vr.value().ok ? 1 : 0,
                    status_name(vr.value().code), vr.value().size);
    } else {
        std::printf("  verify: FAILED code=%s err=%s\n", status_name(vr.error_code()), vr.error_message().c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: storage-fabric <subcommand> [args]\n"
                    "  discover\n"
                    "  object-create <kind> <size> <text>\n"
                    "  object-show <kind> <size> <text>\n"
                    "  plan <size> <replicas>\n"
                    "  publish <size> <text>\n"
                    "  read <size> <text>\n"
                    "  replicate <size> <text>\n"
                    "  move <size> <text>\n"
                    "  restore <size> <text>\n"
                    "  evict <size> <text>\n"
                    "  verify <size> <text>\n"
                    "  explain <size> <text>\n"
                    "  simulate <size> <replicas>\n"
                    "  save <path>\n"
                    "  recover <path>\n"
                    "  benchmark <count> <size>\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const AuthorityEnvelope auth = cli_authority();
    StorageFabric sf;
    sf.set_authority(auth);

    // ---- discover ----
    if (cmd == "discover") {
        auto bid = register_local(sf, "cli-local", "discover");
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        const StorageBackendId backend = bid.value();
        std::printf("== authority ==\n  %s\n", sf.authority().describe().c_str());
        std::printf("== backends ==\n"); print_backend(sf, backend);
        std::printf("== backend explanation ==\n  %s\n", sf.explain_backend(backend).c_str());
        std::printf("== accounting ==\n"); print_accounting(sf.accounting());
        return 0;
    }

    // ---- object-create / object-show (identical demonstration) ----
    if (cmd == "object-create" || cmd == "object-show") {
        if (argc < 5) { std::printf("%s needs <kind> <size> <text>\n", cmd.c_str()); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[3], size)) { std::printf("bad size\n"); return 2; }
        auto bid = register_local(sf, "cli-local", cmd);
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        return demo_roundtrip(sf, bid.value(), argv[2], size, argv[4], true);
    }

    // ---- plan ----
    if (cmd == "plan") {
        if (argc < 4) { std::printf("plan needs <size> <replicas>\n"); return 2; }
        std::uint64_t size = 0; std::uint32_t replicas = 1;
        if (!parse_u64(argv[2], size) || !parse_u32(argv[3], replicas)) { std::printf("bad args\n"); return 2; }
        auto bid = register_local(sf, "cli-local", "plan");
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        Bytes content = build_content(size, "plan");
        auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::NORMAL);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        const StoragePlan sp = sf.plan(sf.make_plan_request(obj_res.value(), replicas));
        std::printf("== plan ==\n"); print_plan(sp, sf);
        std::printf("== accounting ==\n"); print_accounting(sf.accounting());
        return 0;
    }

    // ---- publish (publish existing object to a second local backend) ----
    if (cmd == "publish") {
        if (argc < 4) { std::printf("publish needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto b1 = register_local(sf, "cli-local", "pub-a");
        auto b2 = register_local(sf, "cli-local-b", "pub-b");
        if (b1.failed() || b2.failed()) { std::printf("register failed\n"); return 1; }
        Bytes content = build_content(size, argv[3]);
        auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::NORMAL);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        const ObjectDescriptor obj = obj_res.value();
        auto p1 = sf.publish_to(obj, ByteSpan(content.data(), content.size()), b1.value(), PublishOptions{});
        if (p1.failed()) { std::printf("publish_to(key) failed: %s\n", p1.error_message().c_str()); return 1; }
        std::printf("== initial publish_to backend %s ==\n", backend_label(sf, b1.value()).c_str());
        print_placement(p1.value(), sf);
        PublishOptions opts; opts.authority = auth; opts.dedupe = false;
        auto p2 = sf.publish_to(obj, ByteSpan(content.data(), content.size()), b2.value(), opts);
        if (p2.failed()) { std::printf("publish_to(second) failed: %s\n", p2.error_message().c_str()); return 1; }
        std::printf("== secondary publish_to backend %s ==\n", backend_label(sf, b2.value()).c_str());
        print_placement(p2.value(), sf);
        std::printf("== replication explanation ==\n  %s\n", sf.explain_replication(obj.id).c_str());
        return 0;
    }

    // ---- read ----
    if (cmd == "read") {
        if (argc < 4) { std::printf("read needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto bid = register_local(sf, "cli-local", "read");
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        const int rc = demo_roundtrip(sf, bid.value(), "GENERIC_AI_STATE", size, argv[3], true);
        if (rc) return rc;
        const auto& objs = sf.objects();
        if (!objs.empty()) {
            const ObjectDescriptor& o = objs.front();
            const auto& ap = sf.find_authoritative_placement(o.id);
            std::printf("== authoritative placement ==\n");
            if (ap.ok()) print_placement(ap.value(), sf);
            else std::printf("  <none>\n");
        }
        return 0;
    }

    // ---- replicate ----
    if (cmd == "replicate") {
        if (argc < 4) { std::printf("replicate needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto b1 = register_local(sf, "cli-local", "rep-a");
        auto b2 = register_local(sf, "cli-local-b", "rep-b");
        if (b1.failed() || b2.failed()) { std::printf("register failed\n"); return 1; }
        Bytes content = build_content(size, argv[3]);
        auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::NORMAL);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        auto place = sf.publish_to(obj_res.value(), ByteSpan(content.data(), content.size()),
                                   b1.value(), PublishOptions{});
        if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
        PublishOptions opts; opts.authority = auth; opts.dedupe = false;
        auto rep = sf.replicate(place.value(), b2.value(), opts);
        if (rep.failed()) { std::printf("replicate failed: %s\n", rep.error_message().c_str()); return 1; }
        std::printf("== source placement ==\n"); print_placement(place.value(), sf);
        std::printf("== replicated placement (backend %s) ==\n", backend_label(sf, b2.value()).c_str());
        print_placement(rep.value(), sf);
        std::printf("== replica set ==\n  %s\n", sf.explain_replication(obj_res.value().id).c_str());
        return 0;
    }

    // ---- move ----
    if (cmd == "move") {
        if (argc < 4) { std::printf("move needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto b1 = register_local(sf, "cli-local", "mv-a");
        auto b2 = register_local(sf, "cli-local-b", "mv-b");
        if (b1.failed() || b2.failed()) { std::printf("register failed\n"); return 1; }
        Bytes content = build_content(size, argv[3]);
        auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::NORMAL);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        auto place = sf.publish_to(obj_res.value(), ByteSpan(content.data(), content.size()),
                                   b1.value(), PublishOptions{});
        if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
        PublishOptions opts; opts.authority = auth; opts.dedupe = false;
        auto rep = sf.move(place.value(), b2.value(), opts);
        if (rep.failed()) { std::printf("move failed: %s\n", rep.error_message().c_str()); return 1; }
        std::printf("== source placement (now STALE) ==\n"); print_placement(place.value(), sf);
        std::printf("== moved placement (backend %s) ==\n", backend_label(sf, b2.value()).c_str());
        print_placement(rep.value(), sf);
        std::printf("== placement explanation ==\n  %s\n", sf.explain_placement(obj_res.value().id).c_str());
        return 0;
    }

    // ---- restore ----
    if (cmd == "restore") {
        if (argc < 4) { std::printf("restore needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto bid = register_local(sf, "cli-local", "restore");
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        Bytes content = build_content(size, argv[3]);
        auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::HIGH);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        auto place = sf.publish_to(obj_res.value(), ByteSpan(content.data(), content.size()),
                                   bid.value(), PublishOptions{});
        if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
        std::printf("== restore ==\n  %s\n", sf.explain_restore(obj_res.value().id).c_str());
        std::printf("  %s\n", sf.explain_failure(obj_res.value().id).c_str());
        std::printf("  %s\n", sf.explain_replication(obj_res.value().id).c_str());
        return 0;
    }

    // ---- evict (create two placements, then show the durability guard) ----
    if (cmd == "evict") {
        if (argc < 4) { std::printf("evict needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto b1 = register_local(sf, "cli-local", "ev-a");
        auto b2 = register_local(sf, "cli-local-b", "ev-b");
        if (b1.failed() || b2.failed()) { std::printf("register failed\n"); return 1; }
        Bytes content = build_content(size, argv[3]);
        auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::NORMAL);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        auto p1 = sf.publish_to(obj_res.value(), ByteSpan(content.data(), content.size()),
                                b1.value(), PublishOptions{});
        if (p1.failed()) { std::printf("publish_to(first) failed: %s\n", p1.error_message().c_str()); return 1; }
        PublishOptions o2; o2.authority = auth; o2.dedupe = false;
        auto p2 = sf.publish_to(obj_res.value(), ByteSpan(content.data(), content.size()),
                                b2.value(), o2);
        if (p2.failed()) { std::printf("publish_to(second) failed: %s\n", p2.error_message().c_str()); return 1; }
        std::printf("== two placements created ==\n"); print_placement(p1.value(), sf); print_placement(p2.value(), sf);
        const EvictionDecision pre = sf.can_evict(p1.value().id);
        std::printf("== can_evict ==\n  allowed=%d reason=%s code=%s\n", pre.allowed ? 1 : 0,
                    pre.reason.c_str(), status_name(pre.code));
        auto ev = sf.evict(p1.value().id);
        if (ev.failed()) { std::printf("evict failed: code=%s %s\n", status_name(ev.error_code()), ev.error_message().c_str()); }
        else { std::printf("== evict ==\n  allowed=%d reason=%s\n", ev.value().allowed ? 1 : 0, ev.value().reason.c_str()); }
        std::printf("  explanation: %s\n", sf.explain_eviction(p1.value().id).c_str());
        return 0;
    }

    // ---- verify ----
    if (cmd == "verify") {
        if (argc < 4) { std::printf("verify needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto bid = register_local(sf, "cli-local", "verify");
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        return demo_roundtrip(sf, bid.value(), "GENERIC_AI_STATE", size, argv[3], false);
    }

    // ---- explain ----
    if (cmd == "explain") {
        if (argc < 4) { std::printf("explain needs <size> <text>\n"); return 2; }
        std::uint64_t size = 0;
        if (!parse_u64(argv[2], size)) { std::printf("bad size\n"); return 2; }
        auto bid = register_local(sf, "cli-local", "explain");
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        Bytes content = build_content(size, argv[3]);
        auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::NORMAL);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        auto place = sf.publish_to(obj_res.value(), ByteSpan(content.data(), content.size()),
                                   bid.value(), PublishOptions{});
        if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
        std::printf("== placement explanation ==\n  %s\n", sf.explain_placement(obj_res.value().id).c_str());
        std::printf("== read source ==\n  %s\n", sf.explain_read_source(place.value().id).c_str());
        std::printf("== replication ==\n  %s\n", sf.explain_replication(obj_res.value().id).c_str());
        std::printf("== eviction ==\n  %s\n", sf.explain_eviction(place.value().id).c_str());
        std::printf("== restore ==\n  %s\n", sf.explain_restore(obj_res.value().id).c_str());
        std::printf("== failure ==\n  %s\n", sf.explain_failure(obj_res.value().id).c_str());
        std::printf("== recovery ==\n  %s\n", sf.explain_recovery().c_str());
        return 0;
    }

    // ---- simulate ----
    if (cmd == "simulate") {
        if (argc < 4) { std::printf("simulate needs <size> <replicas>\n"); return 2; }
        std::uint64_t size = 0; std::uint32_t replicas = 1;
        if (!parse_u64(argv[2], size) || !parse_u32(argv[3], replicas)) { std::printf("bad args\n"); return 2; }
        const std::uint64_t GiB = 1024ULL * 1024 * 1024;
        const std::uint64_t MiB = 1024ULL * 1024;
        const double ms = 0.001;
        const double us = 0.000001;

        SyntheticProfile nvme;   // fast NVMe-like
        nvme.storage_class = StorageClass::LOCAL_NVME; nvme.total_bytes = 8 * GiB; nvme.free_bytes = 8 * GiB;
        nvme.read_latency_s = 40 * us; nvme.write_latency_s = 40 * us;
        nvme.read_bps = 6.0 * GiB; nvme.write_bps = 5.0 * GiB;
        nvme.evictable = true; nvme.persistent = true; nvme.failure_domain = "nvme-0";
        nvme.cost_class = "local-nvme"; nvme.locality = "local";

        SyntheticProfile shared;  // slow durable shared filesystem
        shared.storage_class = StorageClass::SHARED_FILESYSTEM; shared.total_bytes = 512 * GiB; shared.free_bytes = 400 * GiB;
        shared.read_latency_s = 1.5 * ms; shared.write_latency_s = 2.0 * ms;
        shared.read_bps = 600 * MiB; shared.write_bps = 300 * MiB;
        shared.evictable = true; shared.persistent = true; shared.failure_domain = "shared-rack-a";
        shared.cost_class = "shared-fs"; shared.locality = "shared";

        SyntheticProfile objst;  // object storage, high latency, huge capacity
        objst.storage_class = StorageClass::OBJECT_STORAGE_CLASS; objst.total_bytes = 64 * GiB; objst.free_bytes = 60 * GiB;
        objst.read_latency_s = 120 * ms; objst.write_latency_s = 250 * ms;
        objst.read_bps = 900 * MiB; objst.write_bps = 250 * MiB;
        objst.evictable = false; objst.persistent = true; objst.failure_domain = "object-store-b";
        objst.cost_class = "object-storage"; objst.locality = "object";

        SyntheticProfile cache;  // constrained cache
        cache.storage_class = StorageClass::MEMORY_STAGING; cache.total_bytes = 64 * MiB; cache.free_bytes = 48 * MiB;
        cache.read_latency_s = 20 * us; cache.write_latency_s = 20 * us;
        cache.read_bps = 12 * GiB; cache.write_bps = 10 * GiB;
        cache.evictable = true; cache.persistent = false; cache.failure_domain = "cache-node";
        cache.cost_class = "memory-cache"; cache.locality = "local";

        SyntheticProfile degraded;
        degraded.storage_class = StorageClass::LOCAL_NVME; degraded.total_bytes = 4 * GiB; degraded.free_bytes = 2 * GiB;
        degraded.read_latency_s = 200 * us; degraded.write_latency_s = 260 * us;
        degraded.read_bps = 2 * GiB; degraded.write_bps = 1 * GiB;
        degraded.health = Health::DEGRADED; degraded.degraded = true; degraded.evictable = true;
        degraded.persistent = true; degraded.failure_domain = "degraded-node";
        degraded.cost_class = "degraded-nvme"; degraded.locality = "local";

        SyntheticProfile unavailable;
        unavailable.storage_class = StorageClass::SYNTHETIC_REMOTE; unavailable.total_bytes = 8 * GiB; unavailable.free_bytes = 8 * GiB;
        unavailable.read_latency_s = 500 * ms; unavailable.write_latency_s = 500 * ms;
        unavailable.read_bps = 1 * MiB; unavailable.write_bps = 1 * MiB;
        unavailable.health = Health::UNAVAILABLE; unavailable.unavailable = true;
        unavailable.evictable = true; unavailable.persistent = false; unavailable.failure_domain = "unavailable-node";
        unavailable.cost_class = "unknown"; unavailable.locality = "remote";

        SyntheticProfile asym;  // asymmetric: slow write, fast read
        asym.storage_class = StorageClass::SYNTHETIC_REMOTE; asym.total_bytes = 16 * GiB; asym.free_bytes = 16 * GiB;
        asym.read_latency_s = 30 * ms; asym.write_latency_s = 500 * ms;
        asym.read_bps = 8 * GiB; asym.write_bps = 40 * MiB;
        asym.evictable = true; asym.persistent = true; asym.failure_domain = "asym-node";
        asym.cost_class = "asymmetric"; asym.locality = "remote";

        SyntheticProfile hb_lc;  // high bandwidth, low capacity
        hb_lc.storage_class = StorageClass::LOCAL_NVME; hb_lc.total_bytes = 64 * MiB; hb_lc.free_bytes = 32 * MiB;
        hb_lc.read_latency_s = 30 * us; hb_lc.write_latency_s = 30 * us;
        hb_lc.read_bps = 15 * GiB; hb_lc.write_bps = 15 * GiB;
        hb_lc.evictable = true; hb_lc.persistent = true; hb_lc.failure_domain = "hblc";
        hb_lc.cost_class = "high-bw-low-cap"; hb_lc.locality = "local";

        SyntheticProfile lb_hc;  // low bandwidth, high capacity
        lb_hc.storage_class = StorageClass::OBJECT_STORAGE_CLASS; lb_hc.total_bytes = 512 * GiB; lb_hc.free_bytes = 500 * GiB;
        lb_hc.read_latency_s = 180 * ms; lb_hc.write_latency_s = 300 * ms;
        lb_hc.read_bps = 30 * MiB; lb_hc.write_bps = 60 * MiB;
        lb_hc.evictable = false; lb_hc.persistent = true; lb_hc.failure_domain = "lbhc";
        lb_hc.cost_class = "low-bw-high-cap"; lb_hc.locality = "remote";

        auto b1 = sf.register_synthetic_backend("nvme-fast", nvme);
        auto b2 = sf.register_synthetic_backend("shared-durable", shared);
        auto b3 = sf.register_synthetic_backend("object-high-latency", objst);
        auto b4 = sf.register_synthetic_backend("constrained-cache", cache);
        auto b5 = sf.register_synthetic_backend("degraded-tier", degraded);
        auto b6 = sf.register_synthetic_backend("unavailable-tier", unavailable);
        auto b7 = sf.register_synthetic_backend("asymmetric-tier", asym);
        auto b8 = sf.register_synthetic_backend("high-bw-low-cap", hb_lc);
        auto b9 = sf.register_synthetic_backend("low-bw-high-cap", lb_hc);

        Result<StorageBackendId> regs[] = { b1, b2, b3, b4, b5, b6, b7, b8, b9 };
        for (auto& r : regs) if (r.failed()) { std::printf("synthetic register failed: %s\n", r.error_message().c_str()); return 1; }

        std::printf("== synthetic backends ==\n");
        for (auto& r : regs) print_backend(sf, r.value());

        Bytes content = build_content(size, "simulate");
        auto obj_res = sf.define_object(ObjectKind::DATASET_SHARD, size,
                                        ByteSpan(content.data(), content.size()), auth,
                                        DurabilityRequirement{}, RestorePriority::HIGH);
        if (obj_res.failed()) { std::printf("define failed: %s\n", obj_res.error_message().c_str()); return 1; }
        const StoragePlan sp = sf.plan(sf.make_plan_request(obj_res.value(), replicas));
        std::printf("== plan over synthetic topology ==\n"); print_plan(sp, sf);
        std::printf("== explanation ==\n  %s\n", sf.explain_placement(obj_res.value().id).c_str());
        return 0;
    }

    // ---- save ----
    if (cmd == "save") {
        if (argc < 3) { std::printf("save needs <path>\n"); return 2; }
        const Status st = sf.save(argv[2]);
        std::printf("== save ==\n  ok=%d code=%s %s\n", st.ok() ? 1 : 0, st.name(), st.message().c_str());
        const AccountingTotals a = sf.accounting();
        std::printf("  accounting_totals: objects=%llu placements=%llu\n",
                    static_cast<unsigned long long>(a.logical_objects),
                    static_cast<unsigned long long>(a.active_placements));
        return st.ok() ? 0 : 1;
    }

    // ---- recover ----
    if (cmd == "recover") {
        if (argc < 3) { std::printf("recover needs <path>\n"); return 2; }
        const Status st = sf.recover(argv[2]);
        std::printf("== recover ==\n  ok=%d code=%s %s\n", st.ok() ? 1 : 0, st.name(), st.message().c_str());
        if (st.ok()) {
            std::printf("  note: %s\n", sf.explain_recovery().c_str());
            std::printf("  restored objects=%zu placements=%zu\n", sf.object_count(), sf.placement_count());
            const AccountingTotals a = sf.accounting();
            std::printf("  accounting: objects=%llu active_placements=%llu participant_restarts=%llu\n",
                        static_cast<unsigned long long>(a.logical_objects),
                        static_cast<unsigned long long>(a.active_placements),
                        static_cast<unsigned long long>(a.participant_restarts));
            for (const auto& o : sf.objects()) {
                std::printf("  object: id=%s gen=%s kind=%s\n", o.id.str().c_str(),
                            o.generation.str().c_str(), to_string(o.kind));
            }
        }
        return st.ok() ? 0 : 1;
    }

    // ---- benchmark ----
    if (cmd == "benchmark") {
        if (argc < 4) { std::printf("benchmark needs <count> <size>\n"); return 2; }
        std::uint64_t count = 0; std::uint64_t size = 0;
        if (!parse_u64(argv[2], count) || !parse_u64(argv[3], size)) { std::printf("bad args\n"); return 2; }
        auto bid = register_local(sf, "cli-local", "bench");
        if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }
        Bytes content = build_content(size, "benchmark");
        const auto start = std::chrono::high_resolution_clock::now();
        for (std::uint64_t i = 0; i < count; ++i) {
            auto obj_res = sf.define_object(ObjectKind::GENERIC_AI_STATE, size,
                                            ByteSpan(content.data(), content.size()), auth,
                                            DurabilityRequirement{}, RestorePriority::NORMAL);
            if (obj_res.failed()) { std::printf("define failed at %llu: %s\n", (unsigned long long)i, obj_res.error_message().c_str()); return 1; }
            auto place = sf.publish_to(obj_res.value(), ByteSpan(content.data(), content.size()),
                                       bid.value(), PublishOptions{});
            if (place.failed()) { std::printf("publish failed at %llu: %s\n", (unsigned long long)i, place.error_message().c_str()); return 1; }
            auto rd = sf.read(obj_res.value().id);
            if (rd.failed()) { std::printf("read failed at %llu: %s\n", (unsigned long long)i, rd.error_message().c_str()); return 1; }
        }
        const auto end = std::chrono::high_resolution_clock::now();
        const double secs = std::chrono::duration<double>(end - start).count();
        const double total_bytes = static_cast<double>(count) * static_cast<double>(size);
        std::printf("== benchmark ==\n  objects=%llu size=%llu elapsed=%.4fs\n",
                    static_cast<unsigned long long>(count), static_cast<unsigned long long>(size), secs);
        std::printf("  ops_per_sec=%.2f write_read_bytes_per_sec=%.2f\n",
                    secs > 0 ? static_cast<double>(count) / secs : 0.0,
                    secs > 0 ? (total_bytes * 2.0) / secs : 0.0);
        const AccountingTotals a = sf.accounting();
        std::printf("  accounting: objects=%llu logical_bytes=%llu physical_bytes=%llu\n",
                    static_cast<unsigned long long>(a.logical_objects),
                    static_cast<unsigned long long>(a.logical_bytes),
                    static_cast<unsigned long long>(a.physical_bytes));
        return 0;
    }

    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    return 2;
}
