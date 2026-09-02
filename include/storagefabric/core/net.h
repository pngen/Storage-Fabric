#pragma once
// Storage Fabric - framed TCP transport for coordinator/worker processes.
// Writes are serialized per connection; no global lock is held during blocking
// I/O. Windows socket shutdown is handled so it cannot deadlock/hang.

#include <cstdint>
#include <string>
#include <vector>

#include "storagefabric/core/status.h"
#include "storagefabric/core/protocol.h"

namespace storagefabric {

namespace netdetail {
// Returns true once Winsock has been initialized (Windows only).
bool ensure_net_initialized();
}

class TcpChannel {
public:
    TcpChannel() = default;
    ~TcpChannel();
    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;
    TcpChannel(TcpChannel&& o) noexcept;
    TcpChannel& operator=(TcpChannel&& o) noexcept;

    // Connects to a host:port (client side).
    Status connect(const std::string& host, std::uint16_t port);

    // Sends a full byte span. Loops on partial writes.
    Status send(ByteSpan data);
    // Receives exactly n bytes. Returns Truncated on peer close.
    Result<Bytes> recv_exact(std::size_t n);

    // Framed send/receive.
    Status send_frame(const Frame& f);
    Result<Frame> recv_frame();

    // Graceful close; never blocks/hangs on shutdown.
    void close() noexcept;
    bool is_open() const noexcept { return open_; }

private:
    friend class TcpListener;
#ifdef _WIN32
    std::uintptr_t fd_{0};   // SOCKET value; 0 means closed
#else
    int fd_{-1};
#endif
    bool open_{false};
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& o) noexcept;
    TcpListener& operator=(TcpListener&& o) noexcept;

    // Binds and listens on host:port.
    Status listen(const std::string& host, std::uint16_t port);
    Result<TcpChannel> accept();   // blocks until a connection arrives
    void close() noexcept;
    std::uint16_t port() const noexcept { return port_; }

private:
#ifdef _WIN32
    std::uintptr_t fd_{0};
#else
    int fd_{-1};
#endif
    std::uint16_t port_{0};
};

}  // namespace storagefabric
