// Storage Fabric - multiprocess WORKER.
//
// Connects to the coordinator over framed TCP, performs the HELLO handshake to
// obtain its (epoch, boot, worker) authority, and then runs a small interactive
// REPL that drives the coordinator RPCs. The worker holds its own authority
// generation, which it increments after each successful mutation. It also
// supports 'commit-stale <epoch> <boot> <gen>' to deliberately replay a stale
// authority envelope, which the coordinator must reject with "stale authority"
// (this is the fencing proof of the multiprocess scenario).
//
// Usage: storage_fabric_worker [port] [host] [workerId]
//        port default 5005, host default 127.0.0.1, workerId default auto.
//
// REPL commands:
//   create <size> <text>            define+publish an object (mutation)
//   read [objectId]                 read object bytes
//   verify [placementId]            verify a placement
//   dispatch <commandId>            begin an in-flight transfer (LEASE/DISPATCH)
//   commit-stale <epoch> <boot> <gen> [cmd]  replay a stale authority (mutation)
//   save [path]                     request a coordinator metadata save
//   recover [path]                  request a coordinator metadata recover
//   ping                            heartbeat
//   exit / quit                     graceful GOODBYE
//
// MANUAL PROOF (see coordinator for the full numbered list):
//   A: create 256 hello-world        -> note object id / placement id
//   B: read <objA> ; verify <placeA>
//   A: save meta.bin
//   A: dispatch 1                    -> in-flight transfer
//   (kill worker A)                  -> coordinator advances epoch
//   A: create 256 hello-2            -> fresh placement, new boot
//   A: commit-stale 1 <oldBoot> 1     -> coordinator rejects "stale authority"
//   A: create 256 payload 2 ; verify; save meta.bin
//   (stop/restart coordinator)       -> recover meta.bin ; read <objA> succeeds

#include "storagefabric/core/net.h"
#include "storagefabric/core/protocol.h"
#include "storagefabric/core/bytes.h"
#include "storagefabric/core/strong.h"
#include "storagefabric/core/digest.h"
#include "storagefabric/model/enums.h"
#include "storagefabric/model/authority.h"
#include "storagefabric/core/status.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace storagefabric;

namespace {

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


void put_authority(ByteWriter& w, const AuthorityEnvelope& a) {
    w.put_u64(a.epoch.value());
    w.put_u64(a.boot.value());
    w.put_u64(a.worker.value());
    w.put_u64(a.generation.value());
    w.put_u8(static_cast<std::uint8_t>(a.origin));
}

Bytes build_content(std::uint64_t size, const std::string& text) {
    Bytes out(static_cast<std::size_t>(size), 0);
    const std::size_t n = static_cast<std::size_t>(size);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    const std::size_t tlen = text.size() < n ? text.size() : n;
    for (std::size_t i = 0; i < tlen; ++i)
        out[i] = static_cast<std::uint8_t>(static_cast<unsigned char>(text[i]));
    return out;
}

struct Reply {
    bool ok{false};
    Bytes result;
    std::string err;
};

// Sends one request frame and reads one reply frame (coordinator replies with
// the same wire kind). The reply body is [ok u8][blob].
Reply rpc(TcpChannel& ch, WireMessageKind kind, const Bytes& payload, const std::string& rpcName) {
    Status s = ch.send_frame(Frame{kind, payload});
    if (s.failed()) return Reply{false, {}, "send(" + rpcName + ") failed: " + s.to_string()};
    auto fr = recv_frame_local(ch);
    if (fr.failed()) return Reply{false, {}, std::string("recv(") + status_name(fr.error_code()) + "): " + fr.error_message()};
    ByteReader r(ByteSpan(fr.value().payload.data(), fr.value().payload.size()));
    std::uint8_t okb = 0;
    ByteSpan body;
    if (!r.read_u8(okb) || !r.read_blob(body))
        return Reply{false, {}, "malformed reply from coordinator"};
    if (okb == 1) return Reply{true, Bytes(body.begin(), body.end()), {}};
    return Reply{false, {}, std::string(reinterpret_cast<const char*>(body.data()), body.size())};
}

bool parse_u64(const std::string& s, std::uint64_t& out) {
    const char* b = s.data();
    const char* e = b + s.size();
    const auto res = std::from_chars(b, e, out);
    return res.ec == std::errc() && res.ptr == e;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 5005;
    std::string host = "127.0.0.1";
    std::uint64_t worker_id = (now_ns() & 0xFFFF) ^ 1;
    if (argc > 1) { std::uint64_t p = 0; if (parse_u64(argv[1], p)) port = static_cast<std::uint16_t>(p); }
    if (argc > 2) host = argv[2];
    if (argc > 3) { std::uint64_t w = 0; if (parse_u64(argv[3], w) && w != 0) worker_id = w; }

    TcpChannel ch;
    auto cs = ch.connect(host, port);
    if (cs.failed()) { std::fprintf(stderr, "worker: connect to %s:%u failed: %s\n", host.c_str(), port, cs.to_string().c_str()); return 1; }

    // ---- handshake ----
    {
        ByteWriter w;
        w.put_u64(worker_id);
        const std::string desc = "worker:" + std::to_string(worker_id) + "@" + host;
        w.put_blob(ByteSpan(reinterpret_cast<const std::uint8_t*>(desc.data()), desc.size()));
        Status s = ch.send_frame(Frame{WireMessageKind::HELLO, w.take()});
        if (s.failed()) { std::fprintf(stderr, "worker: HELLO send failed: %s\n", s.to_string().c_str()); return 1; }
    }
    auto hello = recv_frame_local(ch);
    if (hello.failed()) {
        std::fprintf(stderr, "worker: handshake recv failed: code=%s msg=%s\n",
                     status_name(hello.error_code()), hello.error_message().c_str());
        return 1;
    }
    if (hello.value().kind != WireMessageKind::HELLO) {
        std::fprintf(stderr, "worker: handshake wrong kind=%u\n", static_cast<unsigned>(hello.value().kind));
        return 1;
    }
    ByteReader hr(ByteSpan(hello.value().payload.data(), hello.value().payload.size()));
    std::uint64_t epoch_v = 0, boot_v = 0, worker_v = 0;
    std::uint32_t magic = 0, ackcrc = 0;
    if (!hr.read_u64(epoch_v) || !hr.read_u64(boot_v) || !hr.read_u64(worker_v) ||
        !hr.read_u32(magic) || !hr.read_u32(ackcrc)) {
        std::fprintf(stderr, "worker: handshake reply malformed\n");
        return 1;
    }
    CoordinatorEpoch epoch(epoch_v);
    WorkerBootId boot(boot_v);
    WorkerId got_worker(worker_v);
    AuthorityGeneration gen(1);
    AuthorityOrigin origin = AuthorityOrigin::WORKER;
    std::printf("worker: handshake ok epoch=%s boot=%s worker=%s (magic=0x%08x ack=0x%08x)\n",
                epoch.str().c_str(), boot.str().c_str(), got_worker.str().c_str(), magic, ackcrc);

    std::uint64_t last_object = 0, last_placement = 0;

    std::string line;
    while (true) {
        std::printf("worker[%s]> ", boot.str().c_str());
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        if (cmd == "exit" || cmd == "quit") {
            ch.send_frame(Frame{WireMessageKind::GOODBYE, Bytes{}});
            std::printf("worker: goodbye\n");
            break;
        }
        if (cmd == "ping") {
            Reply r = rpc(ch, WireMessageKind::PING, Bytes{}, "ping");
            std::printf("  pong ok=%d%s%s\n", r.ok ? 1 : 0, r.ok ? "" : " err=", r.ok ? "" : r.err.c_str());
            continue;
        }
        if (cmd == "create") {
            std::uint64_t size = 0;
            std::string text;
            if (!(ss >> size)) { std::printf("  usage: create <size> <text>\n"); continue; }
            std::getline(ss, text);
            if (!text.empty() && text.front() == ' ') text.erase(text.begin());
            const Bytes content = build_content(size, text);
            ByteWriter w;
            put_authority(w, {epoch, boot, got_worker, gen, origin});
            w.put_u8(static_cast<std::uint8_t>(ObjectKind::GENERIC_AI_STATE));
            w.put_u64(size);
            w.put_blob(ByteSpan(content.data(), content.size()));
            Reply r = rpc(ch, WireMessageKind::CREATE_OBJECT, w.take(), "create");
            if (!r.ok) { std::printf("  create FAILED: %s\n", r.err.c_str()); continue; }
            ByteReader rr(ByteSpan(r.result.data(), r.result.size()));
            std::uint64_t oid = 0, obj_gen = 0, pid = 0;
            ByteSpan digest;
            if (!rr.read_u64(oid) || !rr.read_u64(obj_gen) || !rr.read_u64(pid) ||
                !rr.read_bytes(32, digest)) {
                std::printf("  create: malformed result\n"); continue;
            }
            last_object = oid;
            last_placement = pid;
            gen = gen.next();
            std::printf("  created object id=%llu gen=%llu placement=%llu digest=%s\n",
                        static_cast<unsigned long long>(oid), static_cast<unsigned long long>(obj_gen),
                        static_cast<unsigned long long>(pid), to_hex(digest).c_str());
            continue;
        }
        if (cmd == "read") {
            std::uint64_t oid = last_object;
            std::string tok;
            if (ss >> tok && !parse_u64(tok, oid)) { std::printf("  bad object id\n"); continue; }
            ByteWriter w;
            w.put_u64(oid);
            Reply r = rpc(ch, WireMessageKind::READ_OBJECT, w.take(), "read");
            if (!r.ok) { std::printf("  read FAILED: %s\n", r.err.c_str()); continue; }
            ByteReader rr(ByteSpan(r.result.data(), r.result.size()));
            std::uint64_t n = 0;
            ByteSpan data;
            if (!rr.read_u64(n) || !rr.read_blob(data)) { std::printf("  read: malformed result\n"); continue; }
            std::printf("  read object id=%llu size=%llu bytes=[%.40s]\n",
                        static_cast<unsigned long long>(oid), static_cast<unsigned long long>(n),
                        blob_str(data).c_str());
            continue;
        }
        if (cmd == "verify") {
            std::uint64_t pid = last_placement;
            std::string tok;
            if (ss >> tok && !parse_u64(tok, pid)) { std::printf("  bad placement id\n"); continue; }
            ByteWriter w;
            w.put_u64(pid);
            Reply r = rpc(ch, WireMessageKind::VERIFY, w.take(), "verify");
            if (!r.ok) { std::printf("  verify FAILED: %s\n", r.err.c_str()); continue; }
            ByteReader rr(ByteSpan(r.result.data(), r.result.size()));
            std::uint8_t okb = 0;
            std::uint64_t n = 0;
            if (!rr.read_u8(okb) || !rr.read_u64(n)) { std::printf("  verify: malformed result\n"); continue; }
            std::printf("  verify placement=%llu ok=%d size=%llu\n",
                        static_cast<unsigned long long>(pid), okb ? 1 : 0,
                        static_cast<unsigned long long>(n));
            continue;
        }
        if (cmd == "dispatch" || cmd == "lease") {
            std::uint64_t cid = 0;
            if (!(ss >> cid)) { std::printf("  usage: dispatch <commandId>\n"); continue; }
            ByteWriter w;
            put_authority(w, {epoch, boot, got_worker, gen, origin});
            w.put_u64(cid);
            Reply r = rpc(ch, WireMessageKind::COMMIT, w.take(), "dispatch");
            if (!r.ok) { std::printf("  dispatch FAILED: %s\n", r.err.c_str()); continue; }
            ByteReader rr(ByteSpan(r.result.data(), r.result.size()));
            std::uint64_t did = 0, dgen = 0;
            if (!rr.read_u64(did) || !rr.read_u64(dgen)) { std::printf("  dispatch: malformed result\n"); continue; }
            gen = gen.next();
            std::printf("  dispatch cmd=%llu -> dispatch_id=%llu gen=%llu (in-flight transfer)\n",
                        static_cast<unsigned long long>(cid), static_cast<unsigned long long>(did),
                        static_cast<unsigned long long>(dgen));
            continue;
        }
        if (cmd == "commit-stale" || cmd == "replay") {
            // Deliberately replay a stale authority envelope. Used to prove the
            // coordinator's fencing rejects authority that is not strictly newer.
            std::uint64_t ep = 0, bo = 0, ge = 0, cid = 999;
            if (!(ss >> ep >> bo >> ge)) { std::printf("  usage: commit-stale <epoch> <boot> <gen>\n"); continue; }
            std::string tok;
            if (ss >> tok) { if (!parse_u64(tok, cid)) cid = 999; }
            ByteWriter w;
            put_authority(w, {CoordinatorEpoch(ep), WorkerBootId(bo), WorkerId(worker_v),
                              AuthorityGeneration(ge), origin});
            w.put_u64(cid);
            Reply r = rpc(ch, WireMessageKind::COMMIT, w.take(), "commit-stale");
            std::printf("  commit-stale(epoch=%llu boot=%llu gen=%llu) -> ok=%d%s%s\n",
                        static_cast<unsigned long long>(ep), static_cast<unsigned long long>(bo),
                        static_cast<unsigned long long>(ge), r.ok ? 1 : 0,
                        r.ok ? "" : " err=", r.ok ? "" : r.err.c_str());
            continue;
        }
        if (cmd == "save") {
            std::string path;
            ss >> path;
            ByteWriter w;
            w.put_blob(ByteSpan(reinterpret_cast<const std::uint8_t*>(path.data()), path.size()));
            Reply r = rpc(ch, WireMessageKind::PUBLISH, w.take(), "save");
            std::printf("  save %s -> ok=%d%s%s\n", path.c_str(), r.ok ? 1 : 0,
                        r.ok ? "" : " err=", r.ok ? "" : r.err.c_str());
            continue;
        }
        if (cmd == "recover") {
            std::string path;
            ss >> path;
            ByteWriter w;
            w.put_blob(ByteSpan(reinterpret_cast<const std::uint8_t*>(path.data()), path.size()));
            Reply r = rpc(ch, WireMessageKind::RECOVER, w.take(), "recover");
            std::printf("  recover %s -> ok=%d%s%s\n", path.c_str(), r.ok ? 1 : 0,
                        r.ok ? "" : " err=", r.ok ? "" : r.err.c_str());
            continue;
        }
        std::printf("  unknown command '%s' (try create/read/verify/dispatch/commit-stale/save/recover/ping/exit)\n", cmd.c_str());
    }

    ch.close();
    return 0;
}
