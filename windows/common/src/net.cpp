#include "tethercam/net.h"

#include <algorithm>
#include <mutex>

namespace tethercam::net {

bool Init() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    });
    return ok;
}

void Socket::Close() {
    if (s_ != INVALID_SOCKET) {
        closesocket(s_);
        s_ = INVALID_SOCKET;
    }
}

void Socket::Shutdown() {
    if (s_ != INVALID_SOCKET) shutdown(s_, SD_BOTH);
}

bool Socket::SendAll(std::span<const uint8_t> data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, 1 << 20));
        const int n = send(s_, reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool Socket::RecvExact(std::span<uint8_t> data) {
    size_t got = 0;
    while (got < data.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(data.size() - got, 1 << 20));
        const int n = recv(s_, reinterpret_cast<char*>(data.data() + got), chunk, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

int Socket::RecvSome(std::span<uint8_t> data) {
    const int n = recv(s_, reinterpret_cast<char*>(data.data()), static_cast<int>(data.size()), 0);
    return n < 0 ? -1 : n;
}

int Socket::WaitReadable(int timeout_ms) {
    WSAPOLLFD pfd{};
    pfd.fd = s_;
    pfd.events = POLLRDNORM;
    const int r = WSAPoll(&pfd, 1, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    return 1;
}

void Socket::SetNoDelay() {
    BOOL on = TRUE;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

void Socket::SetRecvBuffer(int bytes) {
    setsockopt(s_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

Socket ConnectTcp(const std::string& host, uint16_t port, int timeout_ms) {
    if (!Init()) return {};
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return {};
    Socket result;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        Socket s(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!s.Valid()) continue;
        // Non-blocking connect so a missing service fails fast.
        u_long nb = 1;
        ioctlsocket(s.Get(), FIONBIO, &nb);
        connect(s.Get(), ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        WSAPOLLFD pfd{};
        pfd.fd = s.Get();
        pfd.events = POLLWRNORM;
        if (WSAPoll(&pfd, 1, timeout_ms) == 1 && (pfd.revents & POLLWRNORM)) {
            nb = 0;
            ioctlsocket(s.Get(), FIONBIO, &nb);
            result = std::move(s);
            break;
        }
    }
    freeaddrinfo(res);
    return result;
}

Socket ListenTcp(const std::string& host, uint16_t port) {
    if (!Init()) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return {};
    Socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.Valid()) return {};
    BOOL excl = TRUE;
    setsockopt(s.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    if (bind(s.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return {};
    if (listen(s.Get(), 4) != 0) return {};
    return s;
}

Socket Accept(Socket& listener) { return Socket(accept(listener.Get(), nullptr, nullptr)); }

}  // namespace tethercam::net
