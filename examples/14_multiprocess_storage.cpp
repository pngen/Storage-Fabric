#include "storagefabric/core/runtime.h"
#include "storagefabric/core/net.h"
#include "storagefabric/core/protocol.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

using namespace storagefabric;

// ---- tiny payload encoding helpers (authority + object request) ----
static Bytes encode_authority(const AuthorityEnvelope& a) {
    ByteWriter w;
    w.put_u64(a.epoch.value());
    w.put_u64(a.boot.value());
    w.put_u64(a.worker.value());
    w.put_u64(a.generation.value());
    w.put_u8(static_cast<std::uint8_t>(a.origin));
    return w.take();
}

static bool decode_authority(ByteReader& r, AuthorityEnvelope& a) {
    std::uint64_t ep, bt, wk, gn;
    std::uint8_t origin;
    if (!r.read_u64(ep) || !r.read_u64(bt) || !r.read_u64(wk) || !r.read_u64(gn) || !r.read_u8(origin))
        return false;
    a.epoch = CoordinatorEpoch(ep);
    a.boot = WorkerBootId(bt);
    a.worker = WorkerId(wk);
    a.generation = AuthorityGeneration(gn);
    a.origin = static_cast<AuthorityOrigin>(origin);
    return true;
}

static const char* msg_kind(WireMessageKind k) noexcept {
    switch (k) {
        case WireMessageKind::HELLO: return "HELLO";
        case WireMessageKind::PONG: return "PONG";
        case WireMessageKind::PUBLISH: return "PUBLISH";
        case WireMessageKind::COMMIT: return "COMMIT";
        case WireMessageKind::ERROR: return "ERROR";
        case WireMessageKind::GOODBYE: return "GOODBYE";
        case WireMessageKind::PING: return "PING";
        case WireMessageKind::REGISTER_BACKEND: return "REGISTER_BACKEND";
        case WireMessageKind::CREATE_OBJECT: return "CREATE_OBJECT";
        case WireMessageKind::READ_OBJECT: return "READ_OBJECT";
        case WireMessageKind::VERIFY: return "VERIFY";
        case WireMessageKind::REPLICATE: return "REPLICATE";
        case WireMessageKind::RECOVER: return "RECOVER";
    }
    return "UNKNOWN";
}

// NOTE: the library's TcpChannel::recv_frame() decodes the 16-byte header alone, which
// fails with Truncated for any frame that carries a non-empty payload. We read the full
// frame (header + payload) ourselves and decode the combined buffer with decode_frame(),
// which validates magic/version/CRC. This still uses only real Storage Fabric APIs.
static Result<Frame> recv_frame_full(TcpChannel& ch) {
    auto h = ch.recv_exact(kFrameHeaderSize);
    if (h.failed()) return Result<Frame>::failure(h.error_code(), h.error_message());
    ByteReader hr(ByteSpan(h.value().data(), h.value().size()));
    std::uint32_t magic = 0, len = 0, crc = 0;
    std::uint16_t ver = 0, kind = 0;
    if (!hr.read_u32(magic) || !hr.read_u16(ver) || !hr.read_u16(kind) || !hr.read_u32(len) || !hr.read_u32(crc))
        return Result<Frame>::failure(StatusCode::Malformed, "short header");
    Bytes full;
    full.reserve(kFrameHeaderSize + static_cast<std::size_t>(len));
    full.insert(full.end(), h.value().begin(), h.value().end());
    if (len > 0) {
        auto payload = ch.recv_exact(len);
        if (payload.failed()) return Result<Frame>::failure(payload.error_code(), payload.error_message());
        full.insert(full.end(), payload.value().begin(), payload.value().end());
    }
    std::size_t consumed = 0;
    return decode_frame(ByteSpan(full.data(), full.size()), consumed);
}

static Bytes encode_publish(const AuthorityEnvelope& a, ObjectKind kind, const Bytes& content) {
    ByteWriter w;
    w.put_bytes(encode_authority(a));
    w.put_u8(static_cast<std::uint8_t>(kind));
    w.put_u64(static_cast<std::uint64_t>(content.size()));
    w.put_bytes(ByteSpan(content.data(), content.size()));
    return w.take();
}

int main() {
    const std::uint16_t port = 48650;
    const std::string host = "127.0.0.1";

    StorageFabric sf;
    AuthorityEnvelope baseline;
    baseline.epoch = CoordinatorEpoch(150);
    baseline.boot = WorkerBootId(1);
    baseline.worker = WorkerId(1);
    baseline.generation = AuthorityGeneration(1);
    baseline.origin = AuthorityOrigin::COORDINATOR;
    sf.set_authority(baseline);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex14";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("mp-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }

    std::printf("== In-process coordinator + worker over framed TCP (127.0.0.1:%u) ==\n", port);
    TcpListener listener;
    Status ls = listener.listen(host, port);
    if (ls.failed()) { std::printf("listener failed: %s\n", ls.to_string().c_str()); return 1; }
    std::printf("  coordinator listening on %s:%u\n", host.c_str(), listener.port());

    // ---- worker thread: connects, sends a fresh publish then a stale publish ----
    std::thread worker([&]() {
        TcpChannel ch;
        Status cs = ch.connect(host, port);
        std::printf("  worker: connect -> %s\n", cs.ok() ? "ok" : cs.to_string().c_str());
        if (cs.failed()) return;

        Bytes content(16 * 1024);
        for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 53 + 19) & 0xFF);

        AuthorityEnvelope freshAuth;
        freshAuth.epoch = CoordinatorEpoch(200);   // strictly newer than baseline 150
        freshAuth.boot = WorkerBootId(5000);
        freshAuth.worker = WorkerId(77);
        freshAuth.generation = AuthorityGeneration(1);
        freshAuth.origin = AuthorityOrigin::WORKER;

        AuthorityEnvelope staleAuth;
        staleAuth.epoch = CoordinatorEpoch(199);   // older than the accepted 200
        staleAuth.boot = WorkerBootId(5000);
        staleAuth.worker = WorkerId(77);
        staleAuth.generation = AuthorityGeneration(999999);  // huge local gen, still fenced
        staleAuth.origin = AuthorityOrigin::WORKER;

        Frame hello;
        hello.kind = WireMessageKind::HELLO;
        hello.payload = encode_authority(freshAuth);
        if (!ch.send_frame(hello).ok()) { std::printf("  worker: hello send failed\n"); return; }
        auto ack = recv_frame_full(ch);
        std::printf("  worker: got hello ack kind=%s\n", ack.ok() ? msg_kind(ack.value().kind) : "ERROR");

        // First mutation: FRESH authority -> should be accepted.
        Frame pub1;
        pub1.kind = WireMessageKind::PUBLISH;
        pub1.payload = encode_publish(freshAuth, ObjectKind::CHECKPOINT, content);
        if (!ch.send_frame(pub1).ok()) { std::printf("  worker: fresh publish send failed\n"); return; }
        auto resp1 = recv_frame_full(ch);
        if (resp1.ok() && resp1.value().kind == WireMessageKind::COMMIT) {
            std::printf("  worker: FRESH authority publish -> COMMIT (accepted)\n");
        } else {
            std::printf("  worker: FRESH authority publish -> %s (%s)\n",
                        resp1.ok() ? msg_kind(resp1.value().kind) : "ERROR",
                        resp1.ok() ? "unexpected" : resp1.error_message().c_str());
        }

        // Second mutation: STALE authority -> must be fenced.
        Frame pub2;
        pub2.kind = WireMessageKind::PUBLISH;
        pub2.payload = encode_publish(staleAuth, ObjectKind::CHECKPOINT, content);
        if (!ch.send_frame(pub2).ok()) { std::printf("  worker: stale publish send failed\n"); return; }
        auto resp2 = recv_frame_full(ch);
        if (resp2.ok() && resp2.value().kind == WireMessageKind::ERROR) {
            Frame err = resp2.value();
            ByteReader er(ByteSpan(err.payload.data(), err.payload.size()));
            std::uint32_t code = 0;
            er.read_u32(code);
            std::printf("  worker: STALE authority publish -> ERROR (fenced) code=%s\n",
                        status_name(static_cast<StatusCode>(code)));
        } else {
            std::printf("  worker: STALE authority publish -> %s (expected ERROR)\n",
                        resp2.ok() ? msg_kind(resp2.value().kind) : "ERROR");
        }

        Frame bye;
        bye.kind = WireMessageKind::GOODBYE;
        ch.send_frame(bye);
        ch.close();
        std::printf("  worker: done\n");
    });

    // ---- coordinator: accept, service HELLO + PUBLISH, enforce authority fencing ----
    auto conn = listener.accept();
    if (conn.failed()) { std::printf("accept failed\n"); worker.join(); return 1; }
    TcpChannel ch = std::move(conn.value());

    auto f0 = recv_frame_full(ch);
    std::printf("  coordinator: accepted connection, first frame=%s\n",
                f0.ok() ? msg_kind(f0.value().kind) : "ERROR");
    if (f0.ok()) {
        AuthorityEnvelope helloAuth;
        ByteReader hr(ByteSpan(f0.value().payload.data(), f0.value().payload.size()));
        decode_authority(hr, helloAuth);
        Frame ack;
        ack.kind = WireMessageKind::PONG;
        ch.send_frame(ack);
    }

    // Serve two PUBLISH requests.
    AuthorityEnvelope current = baseline;
    for (int round = 0; round < 2; ++round) {
        auto fr = recv_frame_full(ch);
        if (fr.failed()) { std::printf("  coordinator: recv failed: %s\n", fr.error_message().c_str()); break; }
        const Frame f = fr.value();
        if (f.kind == WireMessageKind::GOODBYE) break;
        if (f.kind != WireMessageKind::PUBLISH) continue;

        ByteReader r(ByteSpan(f.payload.data(), f.payload.size()));
        AuthorityEnvelope incoming;
        decode_authority(r, incoming);
        std::uint8_t kindRaw = 0;
        std::uint64_t size = 0;
        r.read_u8(kindRaw);
        r.read_u64(size);
        const ByteSpan contentSpan = r.rest();
        const ObjectKind kind = static_cast<ObjectKind>(kindRaw);

        if (!incoming.is_strictly_newer_than(current)) {
            // Fenced: the stale authority cannot perform a mutating action.
            std::printf("  coordinator: FENCING mutation from authority %s (older than %s)\n",
                        incoming.describe().c_str(), current.describe().c_str());
            Frame err;
            err.kind = WireMessageKind::ERROR;
            ByteWriter ew;
            ew.put_u32(static_cast<std::uint32_t>(StatusCode::StaleAuthority));
            err.payload = ew.take();
            ch.send_frame(err);
            continue;
        }

        // Accept: define + publish + read back; the authority becomes current.
        DurabilityRequirement dur;
        dur.min_replicas = 1;
        auto obj_res = sf.define_object(kind, size, contentSpan, incoming, dur);
        if (obj_res.failed()) { std::printf("  coordinator: define failed: %s\n", obj_res.error_message().c_str()); }
        PublishOptions opts;
        opts.authority = incoming;
        opts.required_replicas = 1;
        auto pl = sf.publish(obj_res.value(), contentSpan, opts);
        if (pl.failed()) { std::printf("  coordinator: publish failed: %s\n", pl.error_message().c_str()); }
        auto rd = sf.read(obj_res.value().id);
        std::printf("  coordinator: ACCEPTED mutation from %s -> placement %s read_back=%zu\n",
                    incoming.describe().c_str(), pl.value().id.str().c_str(), rd.ok() ? rd.value().size() : 0);
        current = incoming;
        Frame commit;
        commit.kind = WireMessageKind::COMMIT;
        ByteWriter cw;
        cw.put_bytes(ByteSpan(pl.value().digest.bytes.data(), 32));
        commit.payload = cw.take();
        ch.send_frame(commit);
    }

    ch.close();
    listener.close();
    worker.join();

    std::printf("\n== Coordinator catalog after session ==\n");
    std::printf("  objects=%zu placements=%zu\n", sf.object_count(), sf.placement_count());
    std::printf("  current account: logical_objects=%llu active_placements=%llu\n",
                (unsigned long long)sf.accounting().logical_objects,
                (unsigned long long)sf.accounting().active_placements);
    std::printf("  fencing condition enforced by coordinator on stale authority: %s\n",
                current.is_strictly_newer_than(baseline) ? "yes" : "no");

    std::filesystem::remove_all(root, ec);
    std::printf("EX14_OK\n");
    return 0;
}
