#pragma once

// Small blocking Winsock helpers shared by the agent and tools.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace tethercam::net {

// Calls WSAStartup once per process.
bool Init();

class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET s) : s_(s) {}
    Socket(Socket&& o) noexcept : s_(std::exchange(o.s_, INVALID_SOCKET)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            Close();
            s_ = std::exchange(o.s_, INVALID_SOCKET);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { Close(); }

    bool Valid() const { return s_ != INVALID_SOCKET; }
    SOCKET Get() const { return s_; }
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

private:
    SOCKET s_ = INVALID_SOCKET;
};

// Connects to host:port over TCP (IPv4/IPv6). Invalid socket on failure.
Socket ConnectTcp(const std::string& host, uint16_t port, int timeout_ms = 3000);
// Listens on host:port. Invalid socket on failure.
Socket ListenTcp(const std::string& host, uint16_t port);
Socket Accept(Socket& listener);

}  // namespace tethercam::net
