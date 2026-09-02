#include "test_util.h"
#include "storagefabric/core/runtime.h"
#include "storagefabric/core/capacity.h"
#include "storagefabric/core/accounting.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/model/tier.h"

#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

using namespace storagefabric;

static std::vector<std::uint8_t> make_content(std::size_t n, std::uint32_t seed) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<std::uint8_t>((i * 13 + seed) & 0xFF);
    return out;
}

int main() {
    std::printf("test_concurrency starting\n");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-test-conc";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(100);
    auth.worker = WorkerId(7);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);
    auto bid = sf.register_local_backend("local", root, StorageClass::LOCAL_FILESYSTEM);
    CHECK_OK(bid);

    DurabilityRequirement dur;
    dur.min_replicas = 1;
    const std::size_t N = 256;

    // ============ A. sequential publish, concurrent readers ============
    const std::uint32_t K = 8;
    std::vector<ObjectId> ids;
    std::vector<std::vector<std::uint8_t>> contents;
    for (std::uint32_t t = 0; t < K; ++t) {
        const auto content = make_content(N, t);
        ByteSpan span = ByteSpan(content.data(), content.size());
        PublishOptions opts;
        opts.required_replicas = 1;
        auto obj = sf.define_object(ObjectKind::TENSOR, content.size(), span, auth, dur);
        CHECK_OK(obj);
        auto place = sf.publish_to(obj.value(), span, bid.value(), opts);
        CHECK_OK(place);
        ids.push_back(obj.value().id);
        contents.push_back(content);
    }

    std::atomic<int> reader_failures{0};
    std::atomic<std::size_t> readers_done{0};
    const int R = 4;
    std::vector<std::thread> readers;
    for (int r = 0; r < R; ++r) {
        readers.emplace_back([&]() {
            for (int iter = 0; iter < 30; ++iter) {
                for (std::uint32_t t = 0; t < K; ++t) {
                    auto rd = sf.read(ids[t]);
                    if (!rd.ok() || !bytes_eq(rd.value(), contents[t])) {
                        reader_failures.fetch_add(1);
                    }
                }
            }
            readers_done.fetch_add(1);
        });
    }
    while (readers_done.load() < R) std::this_thread::yield();
    for (auto& th : readers) th.join();
    CHECK(reader_failures.load() == 0);
    std::printf("  concurrent readers (4 threads x 30 iters x %u objects) PASS\n", K);

    // ============ B. concurrent publishes (guarded) ============
    std::mutex pub_mutex;
    std::atomic<int> pub_failures{0};
    std::atomic<std::size_t> pub_start{0};
    const int P = 4;
    std::vector<std::thread> pubs;
    for (int p = 0; p < P; ++p) {
        pubs.emplace_back([&, p]() {
            pub_start.fetch_add(1);
            while (pub_start.load() < P) std::this_thread::yield();   // start together
            const auto content = make_content(64, static_cast<std::uint32_t>(100 + p));
            ByteSpan span = ByteSpan(content.data(), content.size());
            std::lock_guard<std::mutex> lg(pub_mutex);   // guard unlocked publish maps
            auto obj = sf.define_object(ObjectKind::KV_STATE, content.size(), span, auth, dur);
            if (obj.failed()) { pub_failures.fetch_add(1); return; }
            PublishOptions opts;
            opts.required_replicas = 1;
            auto place = sf.publish_to(obj.value(), span, bid.value(), opts);
            if (place.failed()) { pub_failures.fetch_add(1); return; }
        });
    }
    for (auto& th : pubs) th.join();
    CHECK(pub_failures.load() == 0);
    std::printf("  concurrent publishes (4 threads, guarded) PASS\n");

    // ============ C. reservation acquire/release contention ============
    {
        ReservationLedger led;   // default overcommit allowance 0
        BackendCapacity cap;
        cap.total_bytes = 1000;
        cap.free_bytes = 1000;
        cap.unknown = false;
        led.register_backend(StorageBackendId(1), cap);

        std::atomic<int> res_failures{0};
        std::atomic<std::size_t> res_start{0};
        const int T = 4;
        std::vector<std::thread> reservers;
        for (int t = 0; t < T; ++t) {
            reservers.emplace_back([&, t]() {
                res_start.fetch_add(1);
                while (res_start.load() < T) std::this_thread::yield();
                for (int i = 0; i < 40; ++i) {
                    const std::uint64_t bytes = 50;
                    auto res = led.reserve(StorageBackendId(1), bytes,
                                           ReservationGeneration(1), WorkerId(t + 1),
                                           "concurrency");
                    if (res.failed()) { res_failures.fetch_add(1); break; }
                    ReservationGuard guard(&led, res.value());
                    // holds the reservation until scope exit, then releases
                }
            });
        }
        for (auto& th : reservers) th.join();
        CHECK(res_failures.load() == 0);
        CHECK_EQ(led.total_reserved(), 0u);       // all released, no accounting leak
        CHECK_EQ(led.total_committed(), 0u);

        // Overcommit is rejected.
        auto over = led.reserve(StorageBackendId(1), 2000, ReservationGeneration(1),
                                WorkerId(9), "overcommit");
        CHECK(over.failed());
        CHECK_EQ(static_cast<int>(over.error_code()), static_cast<int>(StatusCode::InsufficientCapacity));
        std::printf("  reservation contention + overcommit rejection PASS\n");
    }

    // ============ D. eviction vs read ============
    {
        const auto content = make_content(N, 7);
        ByteSpan span = ByteSpan(content.data(), content.size());
        PublishOptions opts;
        opts.required_replicas = 1;
        auto obj = sf.define_object(ObjectKind::CHECKPOINT, content.size(), span, auth, dur);
        CHECK_OK(obj);
        auto place = sf.publish_to(obj.value(), span, bid.value(), opts);
        CHECK_OK(place);

        std::atomic<int> evict_failures{0};
        std::atomic<int> read_failures{0};
        std::atomic<std::size_t> sync{0};
        std::thread reader([&]() {
            sync.fetch_add(1);
            while (sync.load() < 2) std::this_thread::yield();
            for (int i = 0; i < 200; ++i) {
                auto rd = sf.read(obj.value().id);
                if (!rd.ok() || !bytes_eq(rd.value(), content)) read_failures.fetch_add(1);
            }
        });
        std::thread evictor([&]() {
            sync.fetch_add(1);
            while (sync.load() < 2) std::this_thread::yield();
            for (int i = 0; i < 200; ++i) {
                const EvictionDecision d = sf.can_evict(place.value().id);
                if (d.allowed) evict_failures.fetch_add(1);   // must stay rejected
                auto vr = sf.verify(place.value().id);
                if (!vr.ok() || !vr.value().ok) evict_failures.fetch_add(1);
            }
        });
        reader.join();
        evictor.join();
        CHECK(read_failures.load() == 0);
        CHECK(evict_failures.load() == 0);   // evict rejected + verify ok throughout
        auto rd = sf.read(obj.value().id);
        CHECK_OK(rd);
        CHECK(bytes_eq(rd.value(), content));
        std::printf("  eviction-vs-read (concurrent readers + verifier) PASS\n");
    }

    std::filesystem::remove_all(root, ec);
    std::printf("test_concurrency: ALL PASS\n");
    return 0;
}
