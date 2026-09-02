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
    auth.boot = WorkerBootId(11);
    auth.worker = WorkerId(3);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex01";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("identity-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }

    std::vector<std::uint8_t> content(16 * 1024);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<std::uint8_t>((i * 17 + 3) & 0xFF);

    DurabilityRequirement dur;
    dur.min_replicas = 1;

    // Two logical objects with IDENTICAL content => same content digest, distinct identity.
    auto o1 = sf.define_object(ObjectKind::TENSOR, content.size(), ByteSpan(content.data(), content.size()), auth, dur);
    auto o2 = sf.define_object(ObjectKind::TENSOR, content.size(), ByteSpan(content.data(), content.size()), auth, dur);
    // A third logical object with DIFFERENT content => different digest.
    std::vector<std::uint8_t> other(16 * 1024);
    for (size_t i = 0; i < other.size(); ++i) other[i] = static_cast<std::uint8_t>((i * 7 + 5) & 0xFF);
    auto o3 = sf.define_object(ObjectKind::CHECKPOINT, other.size(), ByteSpan(other.data(), other.size()), auth, dur);

    if (o1.failed() || o2.failed() || o3.failed()) {
        std::printf("define failed: %s\n", o1.error_message().c_str());
        return 1;
    }
    const ObjectDescriptor a = o1.value();
    const ObjectDescriptor b = o2.value();
    const ObjectDescriptor c = o3.value();

    std::printf("== Object identity ==\n");
    std::printf("  object A id=%s generation=%s kind=%s owner=%s\n",
                a.id.str().c_str(), a.generation.str().c_str(), to_string(a.kind), a.owner.str().c_str());
    std::printf("  object A digest=%s\n", a.digest.short_hex(16).c_str());
    std::printf("  object B id=%s generation=%s digest=%s\n",
                b.id.str().c_str(), b.generation.str().c_str(), b.digest.short_hex(16).c_str());
    std::printf("  object C id=%s generation=%s kind=%s digest=%s\n",
                c.id.str().c_str(), c.generation.str().c_str(), to_string(c.kind), c.digest.short_hex(16).c_str());

    std::printf("\n  identical content, distinct identity: digest_equal=%s, identity_distinct=%s\n",
                (a.digest == b.digest) ? "yes" : "no", (a.id != b.id) ? "yes" : "no");
    std::printf("  different content, different digest: digest_differs=%s\n",
                !(c.digest == a.digest) ? "yes" : "no");
    std::printf("  provenance A origin=%s worker=%s boot=%s auth_gen=%s\n",
                to_string(a.provenance.origin), a.provenance.worker.str().c_str(),
                a.provenance.boot.str().c_str(), a.provenance.authority_generation.str().c_str());

    // Prove identity is independent of storage path: publish A and read its bytes back.
    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    auto place = sf.publish(a, ByteSpan(content.data(), content.size()), opts);
    if (place.failed()) { std::printf("publish failed: %s\n", place.error_message().c_str()); return 1; }
    auto read = sf.read(a.id);
    if (read.failed()) { std::printf("read failed: %s\n", read.error_message().c_str()); return 1; }
    const bool match = read.value().size() == content.size() &&
                       std::memcmp(read.value().data(), content.data(), content.size()) == 0;
    std::printf("\n  published A (%s) and read back: bytes=%zu match=%s\n",
                a.id.str().c_str(), read.value().size(), match ? "yes" : "no");
    std::printf("  identity A equals placement object: %s\n",
                (place.value().object == a.id) ? "yes" : "no");

    std::filesystem::remove_all(root, ec);
    std::printf("EX01_OK\n");
    return 0;
}
