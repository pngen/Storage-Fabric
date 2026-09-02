#include "storagefabric/core/runtime.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/capacity.h"
#include "storagefabric/core/persist.h"
#include "storagefabric/core/protocol.h"
#include "storagefabric/core/planner.h"
#include "storagefabric/model/enums.h"
#include "storagefabric/model/object.h"
#include "storagefabric/model/manifest.h"
#include "storagefabric/model/placement.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/storage/synthetic_backend.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>
#include <unordered_map>

using namespace storagefabric;

namespace {
constexpr double kMiB = 1024.0 * 1024.0;

template <typename F>
double time_seconds(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / 1000.0;
}

void print_ops(const char* name, std::size_t iters, double secs) {
    const double ops = (secs > 0.0) ? (static_cast<double>(iters) / secs) : 0.0;
    const double us = (iters > 0) ? (secs * 1e6 / static_cast<double>(iters)) : 0.0;
    std::printf("  %-32s %12.1f ops/s   %8.2f us/op\n", name, ops, us);
}

void print_thru(const char* name, std::size_t bytes, double secs) {
    const double mib = (secs > 0.0) ? (static_cast<double>(bytes) / kMiB / secs) : 0.0;
    std::printf("  %-32s %12.1f MiB/s   %8.2f ms/wall\n", name, mib, secs * 1000.0);
}

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint64_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>((i * 31 + seed) & 0xFF);
    return v;
}

TierCandidate make_candidate(StorageBackendId bid, StorageTierId tid, StorageClass cls,
                             double free_b, double total_b, double lat, double wbps,
                             Health health, const std::string& dom, const std::string& cost,
                             MeasurementKind prov) {
    TierCandidate c;
    c.backend = bid; c.tier = tid; c.storage_class = cls;
    c.free_bytes = static_cast<std::uint64_t>(free_b);
    c.total_bytes = static_cast<std::uint64_t>(total_b);
    c.capacity_unknown = false;
    c.read_latency_s = lat; c.write_latency_s = lat;
    c.read_bps = wbps * 4.0; c.write_bps = wbps;
    c.health = health; c.eviction_capable = true;
    c.persistent = (cls == StorageClass::LOCAL_FILESYSTEM);
    c.failure_domain = dom; c.cost_class = cost; c.locality = cost;
    c.current_pressure = 0; c.provenance = prov;
    return c;
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::size_t payload_size = 256 * 1024;
    const std::size_t n_threads = 8;
    std::printf("=== Storage Fabric benchmark (std::chrono::steady_clock) ===\n");
    std::printf("  payload_size=%zu bytes  threads=%zu  local root=%s\n",
                payload_size, n_threads, std::filesystem::temp_directory_path().string().c_str());
    std::printf("  local filesystem timings may include OS page-cache effects; they are NOT claimed "
                "to be physical-disk throughput.\n");

    std::vector<std::uint8_t> payload = make_payload(payload_size, 7);
    const ContentDigest payload_digest = ContentDigest::of(ByteSpan(payload.data(), payload.size()));

    // ---- object descriptor canonicalization (ops/s, us/op) ----
    {
        const std::size_t n = 200000;
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                ObjectDescriptor o;
                o.id = ObjectId(i + 1);
                o.kind = ObjectKind::TENSOR;
                o.logical_size = payload_size;
                o.digest = payload_digest;
                o.owner = OwnerId(i + 1);
                o.durability.min_replicas = 1;
                o.normalize();
                volatile std::uint64_t sink = o.id.value();
                (void)sink;
            }
        });
        std::printf("\n[object descriptor canonicalization]\n");
        print_ops("object descriptor", n, s);
    }

    // ---- SHA-256 (MiB/s) ----
    {
        const std::size_t n = 200;
        const std::size_t bytes = n * payload_size;
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                Bytes d = sha256(ByteSpan(payload.data(), payload.size()));
                volatile std::size_t sink = d.size();
                (void)sink;
            }
        });
        std::printf("\n[SHA-256]\n");
        print_thru("sha256 digest", bytes, s);
        print_ops("sha256 invocations", n, s);
    }

    // ---- manifest creation (ops/s) ----
    {
        const std::size_t chunk_size = 4096;
        const std::size_t nchunks = 16;
        const std::size_t n = 20000;
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                Manifest m;
                m.id = ManifestId(i + 1);
                m.generation = ManifestGeneration(1);
                m.object = ObjectId(1);
                m.object_generation = ObjectGeneration(1);
                m.total_logical_length = nchunks * chunk_size;
                for (std::size_t c = 0; c < nchunks; ++c) {
                    ChunkDescriptor cd;
                    cd.id = ChunkId(c + 1);
                    cd.generation = ChunkGeneration(1);
                    cd.offset = c * chunk_size;
                    cd.logical_length = chunk_size;
                    cd.physical_length = chunk_size;
                    cd.digest = payload_digest;
                    cd.blob = BlobId(c + 1);
                    m.chunks.push_back(cd);
                }
                m.sort_chunks();
                m.manifest_digest = payload_digest;
                volatile bool ok = m.validate().ok();
                (void)ok;
            }
        });
        std::printf("\n[manifest creation]\n");
        print_ops("manifest build+validate", n, s);
    }

    // ---- placement selection (ops/s) ----
    {
        const std::size_t n = 20000;
        std::vector<TierCandidate> candidates;
        for (std::size_t i = 0; i < 8; ++i) {
            const bool local = (i % 2 == 0);
            candidates.push_back(make_candidate(StorageBackendId(i + 1), StorageTierId(i + 1),
                local ? StorageClass::LOCAL_FILESYSTEM : StorageClass::SYNTHETIC_REMOTE,
                8e11, 1e12, local ? 1e-5 : 1e-3, local ? 1e9 : 50.0 * 1024 * 1024,
                Health::HEALTHY, local ? "rack-a" : "rack-b",
                local ? "local" : "synthetic-remote",
                local ? MeasurementKind::MEASURED : MeasurementKind::SYNTHETIC));
        }
        PlanRequest req;
        req.object = ObjectId(1);
        req.object_generation = ObjectGeneration(1);
        req.kind = ObjectKind::CHECKPOINT;
        req.logical_size = payload_size;
        req.required_replicas = 2;
        req.policy_generation = PolicyGeneration(1);
        req.candidates = candidates;
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                StoragePlan sp = plan(req);
                volatile bool f = sp.feasible;
                (void)f;
            }
        });
        std::printf("\n[placement selection]\n");
        print_ops("plan(named factors)", n, s);
    }

    // ---- dedup lookup (ops/s) ----
    {
        const std::size_t n = 200000;
        std::unordered_map<ContentDigest, BlobId> index;
        for (std::size_t i = 0; i < 512; ++i) {
            const Bytes b = make_payload(4096, static_cast<std::uint64_t>(i));
            index.emplace(ContentDigest::of(ByteSpan(b.data(), b.size())), BlobId(i + 1));
        }
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                const Bytes b = make_payload(4096, static_cast<std::uint64_t>(i % 512));
                auto it = index.find(ContentDigest::of(ByteSpan(b.data(), b.size())));
                volatile bool hit = (it != index.end());
                (void)hit;
            }
        });
        std::printf("\n[dedup lookup]\n");
        print_ops("digest->blob lookup", n, s);
    }

    // ---- metadata lookup (ops/s) ----
    {
        const std::size_t n = 200000;
        std::unordered_map<ObjectId, std::size_t> catalog;
        for (std::size_t i = 0; i < 4096; ++i) catalog.emplace(ObjectId(i + 1), i);
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                auto it = catalog.find(ObjectId((i % 4096) + 1));
                volatile bool hit = (it != catalog.end());
                (void)hit;
            }
        });
        std::printf("\n[metadata lookup]\n");
        print_ops("object-id lookup", n, s);
    }

    // ---- reservation cycle (ops/s) ----
    {
        const std::size_t n = 100000;
        ReservationLedger ledger;
        BackendCapacity cap;
        cap.total_bytes = 10ULL * 1024 * 1024 * 1024;
        cap.free_bytes = cap.total_bytes;
        ledger.register_backend(StorageBackendId(1), cap);
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                auto r = ledger.reserve(StorageBackendId(1), 4096, ReservationGeneration(1),
                                        WorkerId(1), "bench");
                if (r.ok()) {
                    ledger.release(r.value().id, r.value().generation);
                }
            }
        });
        std::printf("\n[reservation cycle]\n");
        print_ops("reserve+release", n, s);
    }

    // ---- local sequential write / read (MiB/s) ----
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-bench-local";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        BackendDescriptor bd;
        bd.id = StorageBackendId(1); bd.tier = StorageTierId(1); bd.name = "bench-local";
        bd.generation = BackendGeneration(1); bd.health = Health::HEALTHY;
        bd.freshness = Freshness::CURRENT; bd.provenance = MeasurementKind::MEASURED;
        bd.capabilities = {BackendCapability::kWrite, BackendCapability::kRead,
                           BackendCapability::kDelete, BackendCapability::kFlush,
                           BackendCapability::kAtomicRename, BackendCapability::kPersistent};
        StorageTier t;
        t.id = StorageTierId(1); t.name = "bench-local"; t.storage_class = StorageClass::LOCAL_FILESYSTEM;
        t.failure_domain = "local"; t.durability_class = TierDurabilityClass::LOCAL;
        t.persistent = true; t.eviction_capable = true;
        LocalBackend local_be(bd, t, root);

        const std::size_t writes = 128;
        const std::size_t wbytes = writes * payload_size;
        const double ws = time_seconds([&](){
            for (std::size_t i = 0; i < writes; ++i) {
                const std::string key = "bench/w-" + std::to_string(i);
                volatile auto r = local_be.put(ByteSpan(payload.data(), payload.size()), key);
                (void)r;
            }
        });
        std::printf("\n[LOCAL MEASURED I/O]  (real filesystem; may include OS cache)\n");
        print_thru("local sequential write", wbytes, ws);

        const std::size_t reads = 128;
        const std::size_t rbytes = reads * payload_size;
        const double rs = time_seconds([&](){
            for (std::size_t i = 0; i < reads; ++i) {
                const std::string key = "bench/w-" + std::to_string(i);
                volatile auto r = local_be.read(key);
                (void)r;
            }
        });
        print_thru("local sequential read", rbytes, rs);
        std::filesystem::remove_all(root, ec);
    }

    // ---- persistence serialize / recover (ops/s) ----
    {
        const std::size_t n = 20000;
        MetadataSnapshot snap;
        snap.epoch = CoordinatorEpoch(1);
        AuthorityEnvelope a; a.epoch = CoordinatorEpoch(1); a.boot = WorkerBootId(1);
        a.worker = WorkerId(1); a.generation = AuthorityGeneration(1); a.origin = AuthorityOrigin::RUNNER;
        snap.authority = a;
        snap.policy_generation = PolicyGeneration(1);
        for (std::size_t i = 0; i < 128; ++i) {
            ObjectDescriptor o;
            o.id = ObjectId(i + 1); o.generation = ObjectGeneration(1); o.kind = ObjectKind::TENSOR;
            o.logical_size = payload_size; o.digest = payload_digest; o.owner = OwnerId(i + 1);
            o.provenance.origin = AuthorityOrigin::RUNNER; o.provenance.creator = "bench";
            o.durability.min_replicas = 1; o.policy_generation = PolicyGeneration(1);
            snap.objects.push_back(o);
        }
        Bytes blob;
        const PersistResult pr0 = serialize_snapshot(snap, blob);
        std::printf("\n[persistence]\n");
        std::printf("  (serialize payload=%zu bytes/op  crc=%08X)\n", blob.size(), pr0.crc);
        const double ss = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                Bytes out;
                volatile auto pr = serialize_snapshot(snap, out);
                (void)pr;
            }
        });
        print_ops("persistence serialize (ops/s)", n, ss);

        const double ds = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                MetadataSnapshot out;
                volatile auto pr = deserialize_snapshot(ByteSpan(blob.data(), blob.size()), out);
                (void)pr;
            }
        });
        print_ops("persistence recover (deserialize parser)", n, ds);
        std::printf("  note: in this build deserialize_snapshot rejects its own blob with "
                    "TrailingGarbage/metadata size mismatch (see README limitations); the number "
                    "above is the parse/validate-path throughput.\n");
    }

    // ---- protocol encode/decode (ops/s) ----
    {
        const std::size_t n = 20000;
        std::printf("\n[protocol]\n");
        Bytes enc = encode_frame(WireMessageKind::PUBLISH, ByteSpan(payload.data(), payload.size()));
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                Bytes c = encode_frame(WireMessageKind::PUBLISH, ByteSpan(payload.data(), payload.size()));
                volatile std::size_t sz = c.size();
                (void)sz;
                std::size_t consumed = 0;
                volatile auto d = decode_frame(ByteSpan(enc.data(), enc.size()), consumed);
                (void)d;
            }
        });
        print_ops("protocol encode+decode", n, s);
    }

    // ---- concurrent reads (ops/s) ----
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-bench-conc";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        BackendDescriptor bd;
        bd.id = StorageBackendId(1); bd.tier = StorageTierId(1); bd.name = "bench-conc";
        bd.generation = BackendGeneration(1); bd.health = Health::HEALTHY;
        bd.freshness = Freshness::CURRENT; bd.provenance = MeasurementKind::MEASURED;
        bd.capabilities = {BackendCapability::kWrite, BackendCapability::kRead, BackendCapability::kPersistent};
        StorageTier t;
        t.id = StorageTierId(1); t.name = "bench-conc"; t.storage_class = StorageClass::LOCAL_FILESYSTEM;
        t.failure_domain = "local"; t.durability_class = TierDurabilityClass::LOCAL;
        t.persistent = true; t.eviction_capable = true;
        const std::filesystem::path root_shared = root;
        LocalBackend lb(bd, t, root_shared);
        for (std::size_t i = 0; i < 16; ++i) lb.put(ByteSpan(payload.data(), payload.size()), "bench/r-" + std::to_string(i));

        const std::size_t reads = 20000;
        std::vector<std::thread> pool;
        auto worker = [&](std::size_t tid) {
            std::size_t done = 0;
            for (std::size_t i = 0; i < reads / n_threads; ++i) {
                const std::string key = "bench/r-" + std::to_string((tid + i) % 16);
                volatile auto r = lb.read(key);
                (void)r;
                ++done;
            }
        };
        const double s = time_seconds([&](){
            for (std::size_t t = 0; t < n_threads; ++t) pool.emplace_back(worker, t);
            for (auto& th : pool) th.join();
            pool.clear();
        });
        std::printf("\n[concurrent reads]  threads=%zu\n", n_threads);
        print_ops("concurrent local reads", reads, s);
        std::printf("  (LocalBackend serializes on a per-instance mutex; this is the completed-work "
                    "rate, not a claim of scaling.)\n");
        std::filesystem::remove_all(root, ec);
    }

    // ---- synthetic remote tier access (us/op, SYNTHETIC) ----
    {
        SyntheticProfile profile;
        profile.storage_class = StorageClass::SYNTHETIC_REMOTE;
        profile.read_latency_s = 0.001; profile.write_latency_s = 0.002;
        profile.read_bps = 100.0 * 1024 * 1024; profile.write_bps = 50.0 * 1024 * 1024;
        profile.persistent = false;
        BackendDescriptor bd;
        bd.id = StorageBackendId(1); bd.tier = StorageTierId(1); bd.name = "bench-synth";
        bd.generation = BackendGeneration(1); bd.health = Health::HEALTHY;
        bd.freshness = Freshness::CURRENT; bd.provenance = MeasurementKind::SYNTHETIC;
        StorageTier t;
        t.id = StorageTierId(1); t.name = "bench-synth"; t.storage_class = StorageClass::SYNTHETIC_REMOTE;
        t.failure_domain = "synthetic-node"; t.durability_class = TierDurabilityClass::EPHEMERAL;
        t.persistent = false; t.eviction_capable = true;
        SyntheticBackend syn_be(bd, t, profile);
        const std::size_t writes = 100000;
        const std::size_t n = writes;
        const double s = time_seconds([&](){
            for (std::size_t i = 0; i < n; ++i) {
                const std::string key = "synth/k-" + std::to_string(i % 64);
                volatile auto w = syn_be.put(ByteSpan(payload.data(), payload.size()), key);
                (void)w;
                volatile auto r = syn_be.read(key);
                (void)r;
            }
        });
        std::printf("\n[SYNTHETIC TIER]\n");
        print_ops("synthetic remote put+read (SYNTHETIC)", n, s);
        std::printf("  (in-memory deterministic model; explicitly labeled SYNTHETIC, not real remote I/O.)\n");
    }

    std::printf("\nBENCH_DONE\n");
    return 0;
}
