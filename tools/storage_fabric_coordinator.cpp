// Storage Fabric - multiprocess authority COORDINATOR.
//
// The coordinator owns a single StorageFabric, registers one LOCAL backend on
// startup (root = argv[3] or a temp dir), and listens on 127.0.0.1:<port>
// (argv[1], default 5005) for framed TCP worker connections. One thread per
// worker connection runs a read loop that serves request/reply RPC. Authority
// fencing is enforced: every mutation request (CREATE_OBJECT, COMMIT/dispatch)
// carries an AuthorityEnvelope (epoch, boot, worker, generation) and is accepted
// only when it is strictly newer than the coordinator's current authority (see
// model/authority.h). When a worker's channel closes the coordinator advances
// its coordinator epoch, so any stale (old epoch/boot) request is rejected.
//
// Wire-kind mapping (protocol.h has no dedicated SAVE kind, so PUBLISH is
// repurposed for the save RPC -- both sides agree):
//   HELLO          handshake        REGISTER_BACKEND  register a local backend
//   CREATE_OBJECT  define+publish   READ_OBJECT       read bytes
//   VERIFY         verify placement COMMIT            LEASE/DISPATCH
//   PUBLISH        save (metadata)  RECOVER           recover (metadata)
//   PING           heartbeat        GOODBYE           graceful close
//
// MANUAL PROOF (run by hand; also encoded in worker comments):
//   1.  Coordinator:  storage_fabric_coordinator 5005 meta.bin
//   2.  Worker A:     storage_fabric_worker 5005 127.0.0.1 <workerIdA>
//   3.  Worker B:     storage_fabric_worker 5005 127.0.0.1 <workerIdB>
//   4.  A: create <size> payload
//   5.  B: read <object_id> ; verify <placement_id>
//   6.  A: save meta.bin
//   7.  A: dispatch <cmd>                 (begin in-flight transfer)
//   8.  Kill worker A (Ctrl+C). Coordinator prints "worker loss observed,
//       epoch advanced".
//   9.  Restart worker A (fresh boot). Handshake shows the new epoch/boot.
//  10.  A: commit-stale <oldEpoch> <oldBoot> <oldGen>
//        -> coordinator replies 0 "stale authority" (replayed stale commit).
//  11.  A: create <size> payload 2        (fresh placement, new authority)
//  12.  A: verify <new_placement_id> ; save meta.bin
//  13.  Stop coordinator (Ctrl+C) -> saves metadata then exits.
//  14.  Restart coordinator with meta.bin. Worker A: recover meta.bin, then
//       read <object_id> -> post-recovery read succeeds.

#include "storagefabric/core/runtime.h"
#include "storagefabric/core/net.h"
#include "storagefabric/core/protocol.h"
#include "storagefabric/core/bytes.h"
#include "storagefabric/core/persist.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/core/status.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/model/authority.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace storagefabric;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<std::uint16_t> g_port{0};
std::atomic<std::uint64_t> g_boot_nonce{0};

void on_sigint(int) { g_stop.store(true); }

std::uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count());
}

std::string blob_str(ByteSpan b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Workaround for the library's TcpChannel::recv_frame, which decodes only the
// 16-byte header and therefore rejects any payload-bearing frame ("truncated
// payload"). This reader uses the public recv_exact() to pull the full header +
// payload and validates it with the public decode_frame().
Result<Frame> recv_frame_local(TcpChannel& ch) {
    auto h = ch.recv_exact(kFrameHeaderSize);
    if (h.failed()) return Result<Frame>::failure(h.error_code(), h.error_message());
    ByteReader hr(ByteSpan(h.value().data(), h.value().size()));
    std::uint32_t magic = 0, crc = 0, len = 0;
    std::uint16_t ver = 0, kind = 0;
    if (!hr.read_u32(magic) || !hr.read_u16(ver) || !hr.read_u16(kind) ||
        !hr.read_u32(len) || !hr.read_u32(crc)) {
        return Result<Frame>::failure(StatusCode::Truncated, "bad frame header");
    }
    if (magic != kFrameMagic) return Result<Frame>::failure(StatusCode::ProtocolError, "bad magic");
    if (ver != kProtocolVersion) return Result<Frame>::failure(StatusCode::ProtocolError, "unsupported version");
    if (len > kMaxFramePayload) return Result<Frame>::failure(StatusCode::Overflow, "oversized payload");
    Bytes payload;
    if (len > 0) {
        auto p = ch.recv_exact(len);
        if (p.failed()) return Result<Frame>::failure(p.error_code(), p.error_message());
        payload = std::move(p.value());
    }
    Bytes full;
    full.reserve(kFrameHeaderSize + payload.size());
    full.insert(full.end(), h.value().begin(), h.value().end());
    full.insert(full.end(), payload.begin(), payload.end());
    std::size_t consumed = 0;
    auto df = decode_frame(ByteSpan(full.data(), full.size()), consumed);
    if (df.failed()) return Result<Frame>::failure(df.error_code(), df.error_message());
    return df.value();
}


bool read_authority(ByteReader& r, AuthorityEnvelope& a) {
    std::uint64_t ep = 0, bo = 0, wo = 0, ge = 0;
    std::uint8_t ori = 0;
    if (!r.read_u64(ep) || !r.read_u64(bo) || !r.read_u64(wo) || !r.read_u64(ge) ||
        !r.read_u8(ori)) {
        return false;
    }
    a.epoch = CoordinatorEpoch(ep);
    a.boot = WorkerBootId(bo);
    a.worker = WorkerId(wo);
    a.generation = AuthorityGeneration(ge);
    a.origin = static_cast<AuthorityOrigin>(ori);
    return true;
}

void reply_ok(TcpChannel& ch, WireMessageKind kind, const Bytes& result) {
    ByteWriter w;
    w.put_u8(1);
    w.put_blob(ByteSpan(result.data(), result.size()));
    ch.send_frame(Frame{kind, w.take()});
}

void reply_err(TcpChannel& ch, WireMessageKind kind, const std::string& err) {
    ByteWriter w;
    w.put_u8(0);
    w.put_blob(ByteSpan(reinterpret_cast<const std::uint8_t*>(err.data()), err.size()));
    ch.send_frame(Frame{kind, w.take()});
}

// The coordinator's shared runtime + authority state.
struct Coordinator {
    StorageFabric sf;
    std::mutex auth_mutex;           // guards authority + live_boots
    std::vector<WorkerBootId> live_boots;
    std::atomic<std::uint64_t> dispatch_counter{1};
    std::filesystem::path metadata_path;
    StorageBackendId default_backend;
    std::filesystem::path coord_root;

    void reset_authority_baseline() {
        AuthorityEnvelope a;
        a.epoch = sf.authority().epoch;
        a.boot = WorkerBootId(0);
        a.worker = WorkerId(0);
        a.generation = AuthorityGeneration(0);
        a.origin = AuthorityOrigin::COORDINATOR;
        sf.set_authority(a);
    }

    void on_worker_loss(WorkerBootId boot, std::uint64_t worker) {
        std::lock_guard<std::mutex> lg(auth_mutex);
        for (auto it = live_boots.begin(); it != live_boots.end(); ++it) {
            if (*it == boot) { live_boots.erase(it); break; }
        }
        // Advance the coordinator epoch: any authority carrying the old epoch or
        // the dead worker's boot is now stale and can no longer fence.
        sf.new_coordinator_epoch();
        reset_authority_baseline();
        std::printf("coordinator: worker loss observed, epoch advanced "
                    "(boot=%s worker=%llu epoch=%s)\n",
                    boot.str().c_str(), static_cast<unsigned long long>(worker),
                    sf.authority().epoch.str().c_str());
    }
};

// Handles one worker connection: handshake, then a request/response loop.
void handle_connection(Coordinator& c, TcpChannel ch) {
    auto hello = recv_frame_local(ch);
    if (hello.failed()) {
        std::printf("coordinator: rejected connection (HELLO recv failed): code=%s msg=%s\n",
                    status_name(hello.error_code()), hello.error_message().c_str());
        return;
    }
    if (hello.value().kind != WireMessageKind::HELLO) {
        std::printf("coordinator: rejected connection (HELLO kind=%u)\n",
                    static_cast<unsigned>(hello.value().kind));
        return;
    }
    ByteReader hr(ByteSpan(hello.value().payload.data(), hello.value().payload.size()));
    std::uint64_t worker_id = 0;
    ByteSpan desc;
    if (!hr.read_u64(worker_id) || !hr.read_blob(desc)) {
        std::printf("coordinator: rejected connection (bad handshake fields)\n");
        return;
    }

    // Assign a fresh boot nonce and reply with the current (live) epoch.
    std::uint64_t bv = now_ns() ^ (++g_boot_nonce);
    if (bv == 0) bv = 1;
    const WorkerBootId boot(bv);
    CoordinatorEpoch cur_epoch;
    {
        std::lock_guard<std::mutex> lg(c.auth_mutex);
        cur_epoch = c.sf.authority().epoch;
        c.live_boots.push_back(boot);
    }
    {
        ByteWriter w;
        w.put_u64(cur_epoch.value());
        w.put_u64(boot.value());
        w.put_u64(worker_id);
        w.put_u32(kMetaMagic);
        const std::uint32_t ackcrc = crc32(ByteSpan(reinterpret_cast<const std::uint8_t*>("ACK"), 3));
        w.put_u32(ackcrc);
        ch.send_frame(Frame{WireMessageKind::HELLO, w.take()});
    }
    const std::string descs = blob_str(desc);
    std::printf("coordinator: worker connected (boot=%s worker=%llu desc=%.40s epoch=%s)\n",
                boot.str().c_str(), static_cast<unsigned long long>(worker_id), descs.c_str(),
                cur_epoch.str().c_str());

    bool closing = false;
    while (!closing) {
        auto fr = recv_frame_local(ch);
        if (fr.failed()) break;   // peer closed / truncated -> worker loss
        const Frame f = fr.value();
        ByteReader r(ByteSpan(f.payload.data(), f.payload.size()));

        switch (f.kind) {
            case WireMessageKind::GOODBYE:
                std::printf("coordinator: worker %s sent GOODBYE\n", boot.str().c_str());
                closing = true;
                break;

            case WireMessageKind::PING: {
                Bytes empty;
                reply_ok(ch, WireMessageKind::PONG, empty);
                break;
            }

            case WireMessageKind::REGISTER_BACKEND: {
                ByteSpan name;
                if (!r.read_blob(name)) { reply_err(ch, f.kind, "REGISTER_BACKEND: missing name"); break; }
                const std::string nm = blob_str(name);
                const std::filesystem::path root = c.coord_root / ("wb-" + nm + "-" + boot.str());
                auto bid = c.sf.register_local_backend(nm, root);
                if (bid.failed()) { reply_err(ch, f.kind, bid.error_message()); break; }
                ByteWriter out;
                out.put_u64(bid.value().value());
                reply_ok(ch, f.kind, out.take());
                std::printf("coordinator: registered backend '%s' id=%s\n", nm.c_str(),
                            bid.value().str().c_str());
                break;
            }

            case WireMessageKind::CREATE_OBJECT: {
                AuthorityEnvelope incoming;
                std::uint8_t kind_u8 = 0;
                std::uint64_t size = 0;
                ByteSpan content;
                if (!read_authority(r, incoming) || !r.read_u8(kind_u8) ||
                    !r.read_u64(size) || !r.read_blob(content)) {
                    reply_err(ch, f.kind, "CREATE_OBJECT: malformed body"); break;
                }
                // Authority fencing (mutating request). The check + execute +
                // set_authority are done under the authority lock so a concurrent
                // worker loss cannot interleave and corrupt the epoch.
                std::lock_guard<std::mutex> lg(c.auth_mutex);
                const AuthorityEnvelope cur = c.sf.authority();
                if (!incoming.is_strictly_newer_than(cur)) {
                    std::printf("coordinator: stale authority rejected "
                                "(epoch=%s boot=%s gen=%s) current_epoch=%s\n",
                                incoming.epoch.str().c_str(), incoming.boot.str().c_str(),
                                incoming.generation.str().c_str(), cur.epoch.str().c_str());
                    reply_err(ch, f.kind, "stale authority");
                    break;
                }
                DurabilityRequirement dur;
                dur.min_replicas = 1;
                auto obj_res = c.sf.define_object(static_cast<ObjectKind>(kind_u8), size,
                                                  content, incoming, dur, RestorePriority::HIGH);
                if (obj_res.failed()) { reply_err(ch, f.kind, obj_res.error_message()); break; }
                const ObjectDescriptor obj = obj_res.value();
                PublishOptions opts;
                opts.authority = incoming;
                opts.required_replicas = 1;
                opts.eager_verify = true;
                auto place = c.sf.publish_to(obj, content, c.default_backend, opts);
                if (place.failed()) { reply_err(ch, f.kind, place.error_message()); break; }
                c.sf.set_authority(incoming);
                ByteWriter out;
                out.put_u64(obj.id.value());
                out.put_u64(obj.generation.value());
                out.put_u64(place.value().id.value());
                out.put_bytes(ByteSpan(obj.digest.bytes.data(), 32));
                reply_ok(ch, f.kind, out.take());
                std::printf("coordinator: created object id=%s gen=%s placement=%s\n",
                            obj.id.str().c_str(), obj.generation.str().c_str(),
                            place.value().id.str().c_str());
                break;
            }

            case WireMessageKind::READ_OBJECT: {
                std::uint64_t oid = 0;
                if (!r.read_u64(oid)) { reply_err(ch, f.kind, "READ_OBJECT: missing id"); break; }
                auto rd = c.sf.read(ObjectId(oid));
                if (rd.failed()) { reply_err(ch, f.kind, rd.error_message()); break; }
                ByteWriter out;
                out.put_u64(rd.value().size());
                out.put_blob(ByteSpan(rd.value().data(), rd.value().size()));
                reply_ok(ch, f.kind, out.take());
                std::printf("coordinator: read object id=%llu bytes=%zu\n",
                            static_cast<unsigned long long>(oid), rd.value().size());
                break;
            }

            case WireMessageKind::VERIFY: {
                std::uint64_t pid = 0;
                if (!r.read_u64(pid)) { reply_err(ch, f.kind, "VERIFY: missing placement id"); break; }
                auto vr = c.sf.verify(PlacementId(pid));
                if (vr.failed()) { reply_err(ch, f.kind, vr.error_message()); break; }
                ByteWriter out;
                out.put_u8(vr.value().ok ? 1 : 0);
                out.put_u64(vr.value().size);
                reply_ok(ch, f.kind, out.take());
                std::printf("coordinator: verify placement=%llu ok=%d size=%zu\n",
                            static_cast<unsigned long long>(pid), vr.value().ok ? 1 : 0,
                            vr.value().size);
                break;
            }

            case WireMessageKind::COMMIT: {
                // LEASE / DISPATCH: begin an in-flight transfer. Mutating request.
                AuthorityEnvelope incoming;
                std::uint64_t command_id = 0;
                if (!read_authority(r, incoming) || !r.read_u64(command_id)) {
                    reply_err(ch, f.kind, "COMMIT: malformed body"); break;
                }
                std::lock_guard<std::mutex> lg(c.auth_mutex);
                const AuthorityEnvelope cur = c.sf.authority();
                if (!incoming.is_strictly_newer_than(cur)) {
                    std::printf("coordinator: stale authority rejected "
                                "(epoch=%s boot=%s gen=%s) current_epoch=%s\n",
                                incoming.epoch.str().c_str(), incoming.boot.str().c_str(),
                                incoming.generation.str().c_str(), cur.epoch.str().c_str());
                    reply_err(ch, f.kind, "stale authority");
                    break;
                }
                const std::uint64_t did = c.dispatch_counter.fetch_add(1);
                c.sf.set_authority(incoming);
                ByteWriter out;
                out.put_u64(did);
                out.put_u64(1);   // DispatchGeneration
                reply_ok(ch, f.kind, out.take());
                std::printf("coordinator: dispatch (cmd=%llu) -> dispatch=%llu gen=1\n",
                            static_cast<unsigned long long>(command_id),
                            static_cast<unsigned long long>(did));
                break;
            }

            case WireMessageKind::PUBLISH: {
                // Repurposed as the SAVE RPC (no dedicated kind in protocol.h).
                ByteSpan path;
                if (!r.read_blob(path)) { reply_err(ch, f.kind, "SAVE: missing path"); break; }
                const std::filesystem::path sp = blob_str(path).empty()
                    ? c.metadata_path : std::filesystem::path(blob_str(path));
                Status st;
                st = c.sf.save(sp);
                if (st.failed()) { reply_err(ch, f.kind, st.to_string()); break; }
                Bytes empty;
                reply_ok(ch, f.kind, empty);
                std::printf("coordinator: saved metadata to %s\n", sp.string().c_str());
                break;
            }

            case WireMessageKind::RECOVER: {
                ByteSpan path;
                if (!r.read_blob(path)) { reply_err(ch, f.kind, "RECOVER: missing path"); break; }
                const std::filesystem::path sp = blob_str(path).empty()
                    ? c.metadata_path : std::filesystem::path(blob_str(path));
                Status st;
                st = c.sf.recover(sp);
                if (st.failed()) { reply_err(ch, f.kind, st.to_string()); break; }
                c.reset_authority_baseline();
                Bytes empty;
                reply_ok(ch, f.kind, empty);
                std::printf("coordinator: recovered metadata from %s\n", sp.string().c_str());
                break;
            }

            default:
                reply_err(ch, f.kind, "unsupported request kind");
                break;
        }
    }

    // Connection about to close -> worker loss.
    c.on_worker_loss(boot, worker_id);
}

void watchdog() {
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    TcpChannel probe;
    probe.connect("127.0.0.1", g_port.load());
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 5005;
    if (argc > 1) {
        std::uint64_t p = 0;
        const auto res = std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), p);
        if (res.ec == std::errc()) port = static_cast<std::uint16_t>(p);
    }
    const std::filesystem::path metadata_path = (argc > 2) ? argv[2] : std::filesystem::path();
    const std::filesystem::path root = (argc > 3)
        ? std::filesystem::path(argv[3])
        : (std::filesystem::temp_directory_path() / "storagefabric-coord");

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("coordinator: port=%u root=%s meta=%s\n", port, root.string().c_str(),
                metadata_path.empty() ? "(none)" : metadata_path.string().c_str());

    auto c = std::make_shared<Coordinator>();
    c->metadata_path = metadata_path;
    c->coord_root = root;
    std::error_code ec;
    std::filesystem::create_directories(root, ec);

    // Authority baseline: epoch 1, nil boot, nil generation. The first live
    // worker (nonzero boot, gen>=1) is strictly newer, so it may mutate.
    AuthorityEnvelope seed;
    seed.epoch = CoordinatorEpoch(1);
    seed.boot = WorkerBootId(0);
    seed.worker = WorkerId(0);
    seed.generation = AuthorityGeneration(0);
    seed.origin = AuthorityOrigin::COORDINATOR;
    c->sf.set_authority(seed);

    auto bid = c->sf.register_local_backend("coord-local", root);
    if (bid.failed()) {
        std::printf("coordinator: register_local_backend failed: %s\n", bid.error_message().c_str());
        return 1;
    }
    c->default_backend = bid.value();
    std::printf("coordinator: registered local backend id=%s root=%s\n",
                c->default_backend.str().c_str(), root.string().c_str());

    TcpListener listener;
    auto ls = listener.listen("127.0.0.1", port);
    if (ls.failed()) {
        std::printf("coordinator: listen failed: %s\n", ls.to_string().c_str());
        return 1;
    }
    g_port.store(port);
    std::signal(SIGINT, on_sigint);
    std::thread watcher(watchdog);
    watcher.detach();

    std::printf("coordinator: listening on 127.0.0.1:%u\n", port);

    while (!g_stop.load()) {
        auto ch = listener.accept();
        if (ch.failed()) {
            if (g_stop.load()) break;
            std::printf("coordinator: accept failed: %s\n", ch.status().to_string().c_str());
            break;
        }
        if (g_stop.load()) {
            ch.value().close();
            break;
        }
        std::thread th([c, chv = std::move(ch.value())]() mutable {
            handle_connection(*c, std::move(chv));
        });
        th.detach();
    }

    listener.close();
    if (!c->metadata_path.empty()) {
        const Status st = c->sf.save(c->metadata_path);
        std::printf("coordinator: saved metadata to %s ok=%d\n",
                    c->metadata_path.string().c_str(), st.ok() ? 1 : 0);
        if (st.failed()) std::printf("coordinator: save: %s\n", st.to_string().c_str());
    } else {
        std::printf("coordinator: no metadata path; skipping save\n");
    }
    std::printf("coordinator: exiting\n");
    return 0;
}
