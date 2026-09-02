#include "storagefabric/core/net.h"

#include <cstring>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

using sock_t = SOCKET;
constexpr sock_t kInvalidSocket = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
using sock_t = int;
constexpr sock_t kInvalidSocket = -1;
#endif

namespace storagefabric {

namespace netdetail {
#ifdef _WIN32
bool ensure_net_initialized() {
    static bool init = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return init;
}
#else
bool ensure_net_initialized() { return true; }
#endif
}  // namespace netdetail

namespace {
bool would_block(int res) {
#ifdef _WIN32
    return res == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
#endif
}

Status last_socket_error(std::string_view op) {
#ifdef _WIN32
    const int err = WSAGetLastError();
    return Status(StatusCode::IoError, std::string(op) + " failed, winsock error=" + std::to_string(err));
#else
    return Status(StatusCode::IoError, std::string(op) + " failed: " + std::strerror(errno));
#endif
}

void set_linger_zero(sock_t fd) {
#ifdef _WIN32
    linger l{};
    l.l_onoff = 1;
    l.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
#else
    linger l{};
    l.l_onoff = 1;
    l.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
#endif
}
}  // namespace

// ---------------------------------------------------------------------------
// TcpChannel
// ---------------------------------------------------------------------------
TcpChannel::~TcpChannel() { close(); }

TcpChannel::TcpChannel(TcpChannel&& o) noexcept { *this = std::move(o); }

TcpChannel& TcpChannel::operator=(TcpChannel&& o) noexcept {
    if (this != &o) {
        close();
#ifdef _WIN32
        fd_ = o.fd_;
        o.fd_ = 0;
#else
        fd_ = o.fd_;
        o.fd_ = -1;
#endif
        open_ = o.open_;
        o.open_ = false;
    }
    return *this;
}

Status TcpChannel::connect(const std::string& host, std::uint16_t port) {
    if (!netdetail::ensure_net_initialized()) {
        return Status(StatusCode::KernelError, "Winsock init failed");
    }
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return last_socket_error("socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
            std::memcpy(&addr.sin_addr, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, 4);
            freeaddrinfo(res);
        }
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return last_socket_error("connect");
    }
    fd_ = static_cast<std::uintptr_t>(s);
    open_ = true;
    return Status::ok_status();
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return last_socket_error("socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
            std::memcpy(&addr.sin_addr, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, 4);
            freeaddrinfo(res);
        }
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(s);
        return last_socket_error("connect");
    }
    fd_ = s;
    open_ = true;
    return Status::ok_status();
#endif
}

Status TcpChannel::send(ByteSpan data) {
#ifdef _WIN32
    if (!open_) return Status(StatusCode::InvalidState, "socket not connected");
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(data.data() + sent),
                       static_cast<int>(data.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            return last_socket_error("send");
        }
        sent += static_cast<std::size_t>(n);
    }
    return Status::ok_status();
#else
    if (!open_) return Status(StatusCode::InvalidState, "socket not connected");
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return last_socket_error("send");
        }
        sent += static_cast<std::size_t>(n);
    }
    return Status::ok_status();
#endif
}

Result<Bytes> TcpChannel::recv_exact(std::size_t n) {
    if (!open_) return Result<Bytes>::failure(StatusCode::InvalidState, "socket not connected");
    Bytes out;
    out.resize(n);
    std::size_t got = 0;
#ifdef _WIN32
    while (got < n) {
        int r = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(out.data() + got),
                       static_cast<int>(n - got), 0);
        if (r == 0) return Result<Bytes>::failure(StatusCode::Truncated, "peer closed");
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            return Result<Bytes>::failure(StatusCode::IoError, "recv failed");
        }
        got += static_cast<std::size_t>(r);
    }
#else
    while (got < n) {
        ssize_t r = ::recv(fd_, out.data() + got, n - got, 0);
        if (r == 0) return Result<Bytes>::failure(StatusCode::Truncated, "peer closed");
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return Result<Bytes>::failure(StatusCode::IoError, "recv failed");
        }
        got += static_cast<std::size_t>(r);
    }
#endif
    return out;
}

Status TcpChannel::send_frame(const Frame& f) {
    Bytes raw = encode_frame(f.kind, ByteSpan(f.payload.data(), f.payload.size()));
    if (raw.empty()) return Status(StatusCode::Overflow, "frame too large");
    return send(ByteSpan(raw.data(), raw.size()));
}

Result<Frame> TcpChannel::recv_frame() {
    // Read the fixed 16-byte header, then the payload length it declares, then
    // decode the FULL frame (header+payload) so magic/version/CRC are validated.
    Bytes header;
    {
        auto r = recv_exact(kFrameHeaderSize);
        if (r.failed()) return Result<Frame>::failure(r.error_code(), r.error_message());
        header = std::move(r.value());
    }
    // Parse the declared payload length from header bytes [8..12) little-endian.
    std::uint32_t len = 0;
    for (int i = 0; i < 4; ++i) len |= static_cast<std::uint32_t>(header[8 + i]) << (8 * i);
    if (len > kMaxFramePayload) return Result<Frame>::failure(StatusCode::Overflow, "oversized payload");
    Bytes full;
    full.reserve(kFrameHeaderSize + static_cast<std::size_t>(len));
    full.insert(full.end(), header.begin(), header.end());
    if (len > 0) {
        auto payload = recv_exact(len);
        if (payload.failed()) return Result<Frame>::failure(payload.error_code(), payload.error_message());
        full.insert(full.end(), payload.value().begin(), payload.value().end());
    }
    std::size_t consumed = 0;
    auto df = decode_frame(ByteSpan(full.data(), full.size()), consumed);
    if (df.failed()) return Result<Frame>::failure(df.error_code(), df.error_message());
    return df.value();
}

void TcpChannel::close() noexcept {
#ifdef _WIN32
    if (fd_ != 0) {
        SOCKET s = static_cast<SOCKET>(fd_);
        set_linger_zero(s);
        shutdown(s, SD_BOTH);
        closesocket(s);
        fd_ = 0;
        open_ = false;
    }
#else
    if (fd_ >= 0) {
        set_linger_zero(fd_);
        shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
        open_ = false;
    }
#endif
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------
TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& o) noexcept { *this = std::move(o); }

TcpListener& TcpListener::operator=(TcpListener&& o) noexcept {
    if (this != &o) {
        close();
#ifdef _WIN32
        fd_ = o.fd_; o.fd_ = 0;
#else
        fd_ = o.fd_; o.fd_ = -1;
#endif
        port_ = o.port_;
    }
    return *this;
}

Status TcpListener::listen(const std::string& host, std::uint16_t port) {
    if (!netdetail::ensure_net_initialized()) {
        return Status(StatusCode::KernelError, "Winsock init failed");
    }
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return last_socket_error("socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return last_socket_error("bind");
    }
    if (::listen(s, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(s);
        return last_socket_error("listen");
    }
    fd_ = static_cast<std::uintptr_t>(s);
    port_ = port;
    return Status::ok_status();
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return last_socket_error("socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { ::close(s); return last_socket_error("bind"); }
    if (::listen(s, SOMAXCONN) < 0) { ::close(s); return last_socket_error("listen"); }
    fd_ = s;
    port_ = port;
    return Status::ok_status();
#endif
}

Result<TcpChannel> TcpListener::accept() {
#ifdef _WIN32
    sockaddr_in client{};
    int len = sizeof(client);
    SOCKET s = ::accept(static_cast<SOCKET>(fd_), reinterpret_cast<sockaddr*>(&client), &len);
    if (s == INVALID_SOCKET) return Result<TcpChannel>::failure(StatusCode::IoError, "accept failed");
    TcpChannel ch;
    ch.fd_ = static_cast<std::uintptr_t>(s);
    ch.open_ = true;
    return ch;
#else
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    int s = ::accept(fd_, reinterpret_cast<sockaddr*>(&client), &len);
    if (s < 0) return Result<TcpChannel>::failure(StatusCode::IoError, "accept failed");
    TcpChannel ch;
    ch.fd_ = s;
    ch.open_ = true;
    return ch;
#endif
}

void TcpListener::close() noexcept {
#ifdef _WIN32
    if (fd_ != 0) {
        closesocket(static_cast<SOCKET>(fd_));
        fd_ = 0;
    }
#else
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
#endif
}

}  // namespace storagefabric
