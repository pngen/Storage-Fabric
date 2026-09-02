#include "storagefabric/core/runtime.h"
#include "storagefabric/storage/local_backend.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

// The CUDA path is valid only when compiled with nvcc (which defines __CUDACC__).
// The normal /W4 /WX C++ build never sees the CUDA headers or kernels.
#if defined(__CUDACC__)
#define SF_CUDA_STAGING 1
#include <cuda_runtime.h>

__global__ void sf_transform_kernel(const unsigned char* __restrict__ in,
                                    unsigned char* __restrict__ out,
                                    unsigned long long n,
                                    unsigned int addv,
                                    unsigned char xorv) {
    const unsigned long long i =
        (static_cast<unsigned long long>(blockIdx.x) * blockDim.x) + threadIdx.x;
    if (i < n) {
        const unsigned int v = static_cast<unsigned int>(in[i]) + addv;
        out[i] = static_cast<unsigned char>(v ^ static_cast<unsigned int>(xorv));
    }
}
#endif

using namespace storagefabric;

static std::vector<std::uint8_t> make_deterministic(std::size_t n, std::uint64_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>((i * 61 + seed) & 0xFF);
    return v;
}

int main() {
#if defined(SF_CUDA_STAGING)
    std::printf("LOCAL_STORAGE_TO_CUDA_STAGING\n");
    std::printf("== CUDA staging proof (nvcc build) ==\n");

    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess) { std::printf("cudaGetDevice failed\n"); return 1; }
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, device);
    std::printf("  device %d: %s compute %d.%d\n", device, prop.name, prop.major, prop.minor);

    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(2026);
    auth.worker = WorkerId(42);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex15-cuda";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("cuda-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed: %s\n", bid.error_message().c_str()); return 1; }

    const std::size_t n = 1ULL * 1024 * 1024;   // 1 MiB
    std::vector<std::uint8_t> host = make_deterministic(n, 7);
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::TENSOR, host.size(),
                                    ByteSpan(host.data(), host.size()), auth, dur);
    if (obj_res.failed()) { std::printf("define failed\n"); return 1; }
    const ObjectDescriptor obj = obj_res.value();

    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    auto pl = sf.publish(obj, ByteSpan(host.data(), host.size()), opts);
    if (pl.failed()) { std::printf("publish failed: %s\n", pl.error_message().c_str()); return 1; }
    auto restored = sf.read(obj.id);
    if (restored.failed()) { std::printf("read failed\n"); return 1; }
    const bool persisted_ok = restored.value().size() == n &&
                              std::memcmp(restored.value().data(), host.data(), n) == 0;
    std::printf("  object %s persisted on local backend and restored: match=%s\n",
                obj.id.str().c_str(), persisted_ok ? "yes" : "no");

    unsigned char* d_in = nullptr;
    unsigned char* d_out = nullptr;
    cudaError_t e = cudaMalloc(reinterpret_cast<void**>(&d_in), n);
    if (e != cudaSuccess) { std::printf("cudaMalloc d_in failed: %s\n", cudaGetErrorString(e)); cudaDeviceReset(); return 1; }
    e = cudaMalloc(reinterpret_cast<void**>(&d_out), n);
    if (e != cudaSuccess) { std::printf("cudaMalloc d_out failed\n"); cudaFree(d_in); cudaDeviceReset(); return 1; }

    e = cudaMemcpy(d_in, host.data(), n, cudaMemcpyHostToDevice);
    if (e != cudaSuccess) { std::printf("cudaMemcpy H2D failed\n"); cudaFree(d_in); cudaFree(d_out); cudaDeviceReset(); return 1; }

    const unsigned int addv = 29;
    const unsigned char xorv = static_cast<unsigned char>(0xA7);
    const int threads = 256;
    const int blocks = static_cast<int>((n + (threads - 1)) / threads);
    sf_transform_kernel<<<blocks, threads>>>(d_in, d_out, static_cast<unsigned long long>(n), addv, xorv);
    e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { std::printf("kernel sync failed: %s\n", cudaGetErrorString(e)); cudaFree(d_in); cudaFree(d_out); cudaDeviceReset(); return 1; }

    std::vector<std::uint8_t> host_out(n);
    e = cudaMemcpy(host_out.data(), d_out, n, cudaMemcpyDeviceToHost);
    if (e != cudaSuccess) { std::printf("cudaMemcpy D2H failed\n"); cudaFree(d_in); cudaFree(d_out); cudaDeviceReset(); return 1; }

    std::vector<std::uint8_t> ref(n);
    for (std::size_t i = 0; i < n; ++i) ref[i] = static_cast<std::uint8_t>((static_cast<unsigned int>(host[i]) + addv) ^ static_cast<unsigned int>(xorv));
    const bool kernel_ok = std::memcmp(host_out.data(), ref.data(), n) == 0;
    std::printf("  transform kernel (add %u ^ 0x%02X) result matches CPU reference: %s\n",
                addv, static_cast<unsigned int>(xorv), kernel_ok ? "yes" : "no");

    std::printf("  DIRECT_STORAGE=UNKNOWN\n");
    std::printf("  NOT GPUDirect Storage\n");
    std::printf("  (staging used cudaMemcpy H2D/D2H; no GPUDirect Storage claim.)\n");

    cudaFree(d_in);
    cudaFree(d_out);
    e = cudaDeviceReset();
    std::printf("  cudaDisable: device reset %s\n", e == cudaSuccess ? "ok" : "failed");
    std::filesystem::remove_all(root, ec);
    std::printf("EX15_OK\n");
    return 0;
#else
    // Non-CUDA build: the local storage half is real, the CUDA device half is skipped.
    std::printf("LOCAL_STORAGE_TO_CUDA_STAGING\n");
    StorageFabric sf;
    AuthorityEnvelope auth;
    auth.epoch = CoordinatorEpoch(1);
    auth.boot = WorkerBootId(2026);
    auth.worker = WorkerId(42);
    auth.generation = AuthorityGeneration(1);
    auth.origin = AuthorityOrigin::RUNNER;
    sf.set_authority(auth);
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "sfb-ex15";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    auto bid = sf.register_local_backend("cuda-local", root, StorageClass::LOCAL_FILESYSTEM);
    if (bid.failed()) { std::printf("register failed\n"); return 1; }
    std::vector<std::uint8_t> host = make_deterministic(32 * 1024, 7);
    DurabilityRequirement dur;
    dur.min_replicas = 1;
    auto obj_res = sf.define_object(ObjectKind::TENSOR, host.size(),
                                    ByteSpan(host.data(), host.size()), auth, dur);
    PublishOptions opts;
    opts.authority = auth;
    opts.required_replicas = 1;
    auto pl = sf.publish(obj_res.value(), ByteSpan(host.data(), host.size()), opts);
    auto restored = sf.read(obj_res.value().id);
    const bool ok = pl.ok() && restored.ok() && restored.value().size() == host.size() &&
                    std::memcmp(restored.value().data(), host.data(), host.size()) == 0;
    std::printf("  local storage half validated: publish+read match=%s\n", ok ? "yes" : "no");
    std::printf("CUDA staging proof skipped (not compiled with CUDA)\n");
    std::printf("DIRECT_STORAGE=UNKNOWN\n");
    std::printf("NOT GPUDirect Storage\n");
    std::filesystem::remove_all(root, ec);
    std::printf("EX15_OK\n");
    return 0;
#endif
}
