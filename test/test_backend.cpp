#include "test_util.h"
#include "storagefabric/storage/local_backend.h"
#include "storagefabric/storage/synthetic_backend.h"

#include <filesystem>
#include <vector>
#include <string>

using namespace storagefabric;

int main() {
    std::printf("test_backend starting\n");

    // ===================== Local backend (real filesystem) ===================
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-test-backend";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    BackendDescriptor d;
    d.id = StorageBackendId(1);
    d.tier = StorageTierId(1);
    d.name = "local";
    StorageTier t;
    t.id = StorageTierId(1);
    t.storage_class = StorageClass::LOCAL_FILESYSTEM;
    LocalBackend be(d, t, root);

    // ---- put / read / verify ----
    std::vector<std::uint8_t> content(2048);
    for (std::size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 37 + 11) & 0xFF);
    ByteSpan span = ByteSpan(content.data(), content.size());
    CHECK(!be.is_synthetic());
    const auto p = be.put(span, "x");
    CHECK_OK(p);
    CHECK_EQ(p.value(), static_cast<std::uint64_t>(content.size()));
    const auto rd = be.read("x");
    CHECK_OK(rd);
    CHECK(bytes_eq(rd.value(), content));

    const ContentDigest expect = ContentDigest::of(span);
    const auto v = be.verify("x", expect);
    CHECK_OK(v);
    CHECK(v.value().ok);
    CHECK_EQ(v.value().size, content.size());
    CHECK(v.value().digest == expect);

    const ContentDigest wrong = ContentDigest::of(ByteSpan(reinterpret_cast<const std::uint8_t*>("zz"), 2));
    const auto vb = be.verify("x", wrong);
    CHECK_OK(vb);
    CHECK(!vb.value().ok);
    CHECK_EQ(static_cast<int>(vb.value().code), static_cast<int>(StatusCode::DigestMismatch));
    std::printf("  local put/read/verify PASS\n");

    // ---- path safety ----
    CHECK_EQ(static_cast<int>(validate_governed_key("a/b").code()), static_cast<int>(StatusCode::Ok));
    CHECK_EQ(static_cast<int>(validate_governed_key("../x").code()), static_cast<int>(StatusCode::PathUnsafe));
    CHECK_EQ(static_cast<int>(validate_governed_key("/abs").code()), static_cast<int>(StatusCode::PathUnsafe));
    CHECK_EQ(static_cast<int>(validate_governed_key("x/").code()), static_cast<int>(StatusCode::PathUnsafe));
    CHECK_EQ(static_cast<int>(validate_governed_key("").code()), static_cast<int>(StatusCode::InvalidArgument));
    CHECK_EQ(static_cast<int>(validate_governed_key("a/../b").code()), static_cast<int>(StatusCode::PathUnsafe));
    CHECK_EQ(static_cast<int>(validate_governed_key(".").code()), static_cast<int>(StatusCode::PathUnsafe));
    CHECK_EQ(static_cast<int>(validate_governed_key("a//b").code()), static_cast<int>(StatusCode::PathUnsafe));
    std::string nul_key = "a";
    nul_key.push_back('\0');
    nul_key.push_back('b');
    CHECK_EQ(static_cast<int>(validate_governed_key(nul_key).code()), static_cast<int>(StatusCode::PathUnsafe));

    // Backend operations reject unsafe keys too.
    CHECK_OK(be.put(span, "ok"));
    auto bad1 = be.put(span, "../escape");
    CHECK(!bad1.ok());
    CHECK_EQ(static_cast<int>(bad1.error_code()), static_cast<int>(StatusCode::PathUnsafe));
    auto bad2 = be.put(span, "/absolute");
    CHECK(!bad2.ok());
    CHECK_EQ(static_cast<int>(bad2.error_code()), static_cast<int>(StatusCode::PathUnsafe));
    auto bad3 = be.put(span, "trail/");
    CHECK(!bad3.ok());
    CHECK_EQ(static_cast<int>(bad3.error_code()), static_cast<int>(StatusCode::PathUnsafe));
    std::printf("  local path-safety PASS\n");

    // ---- enumerate / size / exists / remove / capacity ----
    CHECK_OK(be.put(span, "a"));
    CHECK_OK(be.put(span, "nested/b"));
    const auto enum_res = be.enumerate();
    CHECK_OK(enum_res);
    bool found_a = false, found_b = false;
    for (const auto& k : enum_res.value()) {
        if (k == "a") found_a = true;
        if (k == "nested/b") found_b = true;
    }
    CHECK(found_a);
    CHECK(found_b);

    const auto sz = be.size("a");
    CHECK_OK(sz);
    CHECK_EQ(sz.value(), static_cast<std::uint64_t>(content.size()));
    const auto ex = be.exists("a");
    CHECK_OK(ex);
    CHECK(ex.value());
    const auto ex2 = be.exists("missing");
    CHECK_OK(ex2);
    CHECK(!ex2.value());
    const auto rm = be.remove("a");
    CHECK_STATUS(rm);
    const auto ex3 = be.exists("a");
    CHECK_OK(ex3);
    CHECK(!ex3.value());
    const auto cap = be.query_capacity();
    CHECK_OK(cap);
    CHECK(!cap.value().unknown);
    CHECK(cap.value().total_bytes > 0);
    std::printf("  local enumerate/size/exists/remove/capacity PASS\n");

    std::filesystem::remove_all(root, ec);

    // ===================== Synthetic backend ================================
    BackendDescriptor sd;
    sd.id = StorageBackendId(2);
    sd.tier = StorageTierId(2);
    sd.name = "synthetic";
    sd.health = Health::HEALTHY;
    sd.freshness = Freshness::CURRENT;
    sd.provenance = MeasurementKind::UNKNOWN;   // constructor promotes to SYNTHETIC
    StorageTier st;
    st.id = StorageTierId(2);
    st.storage_class = StorageClass::SYNTHETIC_REMOTE;
    SyntheticProfile sp;
    sp.total_bytes = 1000;
    sp.free_bytes = 1000;
    sp.health = Health::HEALTHY;
    SyntheticBackend sb(sd, st, sp);

    // ---- provenance ----
    CHECK(sb.is_synthetic());
    CHECK_EQ(static_cast<int>(sb.descriptor().provenance), static_cast<int>(MeasurementKind::SYNTHETIC));
    std::printf("  synthetic provenance SYNTHETIC PASS\n");

    // ---- capacity enforcement ----
    std::vector<std::uint8_t> big(900, 0x5A);
    std::vector<std::uint8_t> med(200, 0x6B);
    const auto sput = sb.put(ByteSpan(big.data(), big.size()), "k1");
    CHECK_OK(sput);
    CHECK_EQ(sput.value(), 900u);
    const auto over = sb.put(ByteSpan(med.data(), med.size()), "k2");
    CHECK(!over.ok());
    CHECK_EQ(static_cast<int>(over.error_code()), static_cast<int>(StatusCode::InsufficientCapacity));
    const auto scap = sb.query_capacity();
    CHECK_OK(scap);
    CHECK_EQ(scap.value().committed_bytes, 900u);   // matches used_bytes
    std::printf("  synthetic capacity enforcement PASS\n");

    // ---- degraded / unavailable ----
    const auto okread = sb.read("k1");
    CHECK_OK(okread);
    CHECK(bytes_eq(okread.value(), big));

    sb.set_health(Health::DEGRADED, true, false);
    CHECK_EQ(static_cast<int>(sb.put(ByteSpan(med.data(), med.size()), "k3").error_code()),
             static_cast<int>(StatusCode::BackendDegraded));
    const auto dgraderead = sb.read("k1");   // reads still allowed when degraded
    CHECK_OK(dgraderead);
    CHECK(bytes_eq(dgraderead.value(), big));

    sb.set_health(Health::UNAVAILABLE, false, true);
    CHECK_EQ(static_cast<int>(sb.put(ByteSpan(med.data(), med.size()), "k4").error_code()),
             static_cast<int>(StatusCode::BackendUnavailable));
    CHECK_EQ(static_cast<int>(sb.read("k1").error_code()),
             static_cast<int>(StatusCode::BackendUnavailable));

    sb.set_health(Health::HEALTHY, false, false);
    const auto rm_k1 = sb.remove("k1");   // free the 900 bytes again
    CHECK_STATUS(rm_k1);
    CHECK_OK(sb.put(ByteSpan(med.data(), med.size()), "k5"));
    std::printf("  synthetic degraded/unavailable PASS\n");

    std::printf("test_backend: ALL PASS\n");
    return 0;
}
