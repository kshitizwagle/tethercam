#pragma once

// Small blocking socket helpers shared by the agent and tools (Winsock on Windows, BSD sockets elsewhere).

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace tethercam::net {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Calls WSAStartup once per process on Windows; ignores SIGPIPE elsewhere.
bool Init();

class Socket {
public:
    Socket() = default;
    explicit Socket(SocketHandle s) : s_(s) {}
    Socket(Socket&& o) noexcept : s_(std::exchange(o.s_, kInvalidSocket)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            Close();
            s_ = std::exchange(o.s_, kInvalidSocket);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { Close(); }

    bool Valid() const { return s_ != kInvalidSocket; }
    SocketHandle Get() const { return s_; }
    void Close();
    // Wakes up a thread blocked in recv on this socket.
    void Shutdown();

    bool SendAll(std::span<const uint8_t> data);
    // Reads exactly data.size() bytes. False on error or EOF.
    bool RecvExact(std::span<uint8_t> data);
    // Reads up to data.size() bytes. Returns bytes read, 0 on EOF, -1 on error.
    int RecvSome(std::span<uint8_t> data);
    // Waits for readability. Returns 1 readable, 0 timeout, -1 error.
    int WaitReadable(int timeout_ms);
    void SetNoDelay();
    void SetRecvBuffer(int bytes);
    // Port this socket is bound to (e.g. after ListenTcp(..., 0)); 0 on error.
    uint16_t LocalPort() const;

private:
    SocketHandle s_ = kInvalidSocket;
};

// Connects to host:port over TCP (IPv4/IPv6). Invalid socket on failure.
Socket ConnectTcp(const std::string& host, uint16_t port, int timeout_ms = 3000);
// Listens on host:port. Invalid socket on failure.
Socket ListenTcp(const std::string& host, uint16_t port);
Socket Accept(Socket& listener);
// Connects to a Unix domain socket (usbmuxd on Linux/macOS). Invalid socket on failure or on Windows.
Socket ConnectUnix(const std::string& path);

}  // namespace tethercam::net
