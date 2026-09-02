#include "storagefabric/storage/local_backend.h"

#include <fstream>
#include <vector>
#include <cstring>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace storagefabric {

namespace {
// Deterministic, process-wide counter for temp file name uniqueness.
std::atomic<std::uint64_t> g_temp_counter{0};

// Flushes the file to durable storage where the platform allows it.
Status flush_file_to_disk(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::wstring wp = p.wstring();
    // FlushFileBuffers requires write access on the handle.
    HANDLE h = CreateFileW(wp.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return Status(StatusCode::IoError, "CreateFileW failed during flush");
    }
    BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) return Status(StatusCode::IoError, "FlushFileBuffers failed");
    return Status::ok_status();
#else
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return Status(StatusCode::IoError, "open failed during flush");
    const int r = ::fsync(fd);
    ::close(fd);
    if (r != 0) return Status(StatusCode::IoError, "fsync failed");
    return Status::ok_status();
#endif
}

// Atomically replaces dest with source. On Windows uses MoveFileEx with
// MOVEFILE_REPLACE_EXISTING; elsewhere std::filesystem::rename.
Status replace_file_atomically(const std::filesystem::path& source,
                               const std::filesystem::path& dest) {
#ifdef _WIN32
    if (!MoveFileExW(source.wstring().c_str(), dest.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        return Status(StatusCode::IoError, "MoveFileEx failed for atomic rename");
    }
    return Status::ok_status();
#else
    std::error_code ec;
    std::filesystem::rename(source, dest, ec);
    if (ec) return Status(StatusCode::IoError, "rename failed: " + ec.message());
    return Status::ok_status();
#endif
}

std::filesystem::path to_native(const std::string& key) {
    // Replace '/' with the platform separator; keys are validated separately.
    std::string k = key;
#ifdef _WIN32
    for (auto& c : k) if (c == '/') c = '\\';
#endif
    return std::filesystem::path(k);
}
}  // namespace

Status validate_governed_key(std::string_view key) noexcept {
    if (key.empty()) return Status(StatusCode::InvalidArgument, "empty key");
    if (key.size() > 512) return Status(StatusCode::InvalidArgument, "key too long");
    if (key.front() == '/' || key.front() == '\\') {
        return Status(StatusCode::PathUnsafe, "absolute key");
    }
    if (key.find('\0') != std::string_view::npos) {
        return Status(StatusCode::PathUnsafe, "key contains NUL");
    }
    if (key.back() == '/' || key.back() == '\\') {
        return Status(StatusCode::PathUnsafe, "key ends with a separator");
    }
    // Must be a single, well-formed relative path component sequence with no '..'.
    std::size_t start = 0;
    while (start < key.size()) {
        const std::size_t slash = key.find_first_of("/\\", start);
        const std::size_t end = (slash == std::string_view::npos) ? key.size() : slash;
        const std::string_view comp = key.substr(start, end - start);
        if (comp.empty() || comp == "." || comp == "..") {
            return Status(StatusCode::PathUnsafe, "unsafe key component");
        }
        if (comp.size() > 128) return Status(StatusCode::InvalidArgument, "key component too long");
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return Status::ok_status();
}

LocalBackend::LocalBackend(BackendDescriptor descriptor, StorageTier tier, std::filesystem::path root)
    : desc_(std::move(descriptor)), tier_(std::move(tier)), root_(std::move(root)) {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    // Create the governed object namespace subdirectory.
    std::filesystem::create_directories(root_ / "objects", ec);
}

Result<std::filesystem::path> LocalBackend::resolve_key(const std::string& key) const {
    const Status k = validate_governed_key(key);
    if (k.failed()) return k;
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::absolute(root_, ec);
    if (ec) return Status(StatusCode::IoError, "cannot resolve backend root");
    const std::filesystem::path target = (base / "objects" / to_native(key)).lexically_normal();
    // Ensure the canonical parent stays under the root.
    const std::filesystem::path root_canon = base.lexically_normal();
    const std::string root_prefix = root_canon.lexically_normal().string();
    std::error_code ec2;
    const auto parent_full = std::filesystem::absolute(target.parent_path(), ec2);
    const std::string parent_str = parent_full.lexically_normal().string();
    if (parent_str.compare(0, root_prefix.size(), root_prefix) != 0) {
        return Status(StatusCode::PathUnsafe, "key escapes backend root");
    }
    std::error_code ec3;
    std::filesystem::create_directories(target.parent_path(), ec3);
    return target;
}

Result<Bytes> LocalBackend::read_file(const std::filesystem::path& p) const {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) return Status(StatusCode::NotFound, "file not found: " + ec.message());
    if (sz > static_cast<std::uintmax_t>(64ull * 1024 * 1024 * 1024)) {
        return Status(StatusCode::Overflow, "file exceeds bounded read");
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) return Status(StatusCode::IoError, "cannot open file for read");
    Bytes out(static_cast<std::size_t>(sz));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz));
    if (!in && in.gcount() != static_cast<std::streamsize>(sz)) {
        return Status(StatusCode::Truncated, "truncated file read");
    }
    return out;
}

Result<std::uint64_t> LocalBackend::put(ByteSpan data, const std::string& key) {
    std::lock_guard<std::mutex> guard(io_lock_);
    const auto target_res = resolve_key(key);
    if (target_res.failed()) return Status(target_res.error_code(), target_res.error_message());
    const std::filesystem::path target = target_res.value();

    const std::filesystem::path temp =
        target.parent_path() / ("." + target.filename().string() + ".tmp." + std::to_string(g_temp_counter.fetch_add(1)));

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return Status(StatusCode::IoError, "cannot create temp file");
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return Status(StatusCode::IoError, "temp write failed");
        }
    }
    const Status dur = flush_file_to_disk(temp);
    if (dur.failed()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return dur;
    }
    const Status rep = replace_file_atomically(temp, target);
    if (rep.failed()) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return rep;
    }
    return static_cast<std::uint64_t>(data.size());
}

Result<Bytes> LocalBackend::read(const std::string& key) const {
    std::lock_guard<std::mutex> guard(io_lock_);
    const auto target_res = resolve_key(key);
    if (target_res.failed()) return Status(target_res.error_code(), target_res.error_message());
    return read_file(target_res.value());
}

Status LocalBackend::remove(const std::string& key) {
    std::lock_guard<std::mutex> guard(io_lock_);
    const auto target_res = resolve_key(key);
    if (target_res.failed()) return Status(target_res.error_code(), target_res.error_message());
    std::error_code ec;
    const bool existed = std::filesystem::remove(target_res.value(), ec);
    if (ec) return Status(StatusCode::IoError, "remove failed: " + ec.message());
    (void)existed;
    return Status::ok_status();
}

Result<bool> LocalBackend::exists(const std::string& key) const {
    std::lock_guard<std::mutex> guard(io_lock_);
    const auto target_res = resolve_key(key);
    if (target_res.failed()) return Status(target_res.error_code(), target_res.error_message());
    std::error_code ec;
    return std::filesystem::exists(target_res.value(), ec);
}

Result<std::uint64_t> LocalBackend::size(const std::string& key) const {
    std::lock_guard<std::mutex> guard(io_lock_);
    const auto target_res = resolve_key(key);
    if (target_res.failed()) return Status(target_res.error_code(), target_res.error_message());
    std::error_code ec;
    const auto sz = std::filesystem::file_size(target_res.value(), ec);
    if (ec) return Status(StatusCode::NotFound, "file not found");
    return static_cast<std::uint64_t>(sz);
}

Result<std::vector<std::string>> LocalBackend::enumerate() const {
    std::lock_guard<std::mutex> guard(io_lock_);
    std::error_code ec;
    std::vector<std::string> out;
    const std::filesystem::path dir = root_ / "objects";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::filesystem::path rel = std::filesystem::relative(entry.path(), dir, ec);
#ifdef _WIN32
            std::string s = rel.generic_string();
#else
            std::string s = rel.generic_string();
#endif
            out.push_back(s);
        }
        if (ec) break;
    }
    return out;
}

Result<BackendCapacity> LocalBackend::query_capacity() const {
    std::error_code ec;
    const auto space = std::filesystem::space(root_, ec);
    if (ec) {
        BackendCapacity cap;
        cap.unknown = true;
        return cap;
    }
    BackendCapacity cap;
    cap.total_bytes = static_cast<std::uint64_t>(space.capacity);
    cap.free_bytes = static_cast<std::uint64_t>(space.available);
    return cap;
}

Result<VerifyResult> LocalBackend::verify(const std::string& key, ContentDigest expect) const {
    const auto data_res = read(key);
    if (data_res.failed()) return Status(data_res.error_code(), data_res.error_message());
    const Bytes& data = data_res.value();
    ContentDigest got = ContentDigest::of(ByteSpan(data.data(), data.size()));
    VerifyResult v;
    v.size = data.size();
    v.digest = got;
    if (!expect.is_zero() && !(got == expect)) {
        v.ok = false;
        v.code = StatusCode::DigestMismatch;
        return v;
    }
    v.ok = true;
    v.code = StatusCode::Ok;
    return v;
}

Status LocalBackend::flush() {
    std::lock_guard<std::mutex> guard(io_lock_);
    return Status::ok_status();
}

}  // namespace storagefabric
