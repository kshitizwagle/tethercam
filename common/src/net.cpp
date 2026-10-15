#include "tethercam/net.h"

#include <algorithm>
#include <mutex>

#ifdef _WIN32
#define TCAM_POLL WSAPoll
using PollFd = WSAPOLLFD;
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/un.h>
#include <unistd.h>
#define TCAM_POLL poll
using PollFd = pollfd;
#endif

namespace tethercam::net {
namespace {

void SetNonBlocking(SocketHandle s, bool on) {
#ifdef _WIN32
    u_long nb = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &nb);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

// Waits for a non-blocking connect to finish; true when it succeeded.
bool FinishConnect(SocketHandle s, int timeout_ms) {
    PollFd pfd{};
    pfd.fd = s;
    pfd.events = POLLOUT;
    if (TCAM_POLL(&pfd, 1, timeout_ms) != 1 || !(pfd.revents & POLLOUT)) return false;
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    return err == 0;
}

}  // namespace

bool Init() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        // A peer that disappears mid-send must return an error, not kill the process.
        signal(SIGPIPE, SIG_IGN);
        ok = true;
#endif
    });
    return ok;
}

void Socket::Close() {
    if (s_ != kInvalidSocket) {
#ifdef _WIN32
        closesocket(s_);
#else
        ::close(s_);
#endif
        s_ = kInvalidSocket;
    }
}

void Socket::Shutdown() {
#ifdef _WIN32
    if (s_ != kInvalidSocket) shutdown(s_, SD_BOTH);
#else
    if (s_ != kInvalidSocket) shutdown(s_, SHUT_RDWR);
#endif
}

bool Socket::SendAll(std::span<const uint8_t> data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, 1 << 20));
        const auto n = send(s_, reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool Socket::RecvExact(std::span<uint8_t> data) {
    size_t got = 0;
    while (got < data.size()) {
        const int chunk = static_cast<int>(std::min<size_t>(data.size() - got, 1 << 20));
        const auto n = recv(s_, reinterpret_cast<char*>(data.data() + got), chunk, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

int Socket::RecvSome(std::span<uint8_t> data) {
    const auto n = recv(s_, reinterpret_cast<char*>(data.data()), static_cast<int>(data.size()), 0);
    return n < 0 ? -1 : static_cast<int>(n);
}

int Socket::WaitReadable(int timeout_ms) {
    PollFd pfd{};
    pfd.fd = s_;
    pfd.events = POLLIN;
    const int r = TCAM_POLL(&pfd, 1, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    return 1;
}

void Socket::SetNoDelay() {
    int on = 1;
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
        SetNonBlocking(s.Get(), true);
        connect(s.Get(), ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (FinishConnect(s.Get(), timeout_ms)) {
            SetNonBlocking(s.Get(), false);
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
#ifdef _WIN32
    BOOL excl = TRUE;
    setsockopt(s.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#else
    // Unix binds are exclusive by default; SO_REUSEADDR only skips TIME_WAIT after a restart.
    int reuse = 1;
    setsockopt(s.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    if (bind(s.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return {};
    if (listen(s.Get(), 4) != 0) return {};
    return s;
}

Socket Accept(Socket& listener) { return Socket(accept(listener.Get(), nullptr, nullptr)); }

Socket ConnectUnix(const std::string& path) {
#ifdef _WIN32
    (void)path;
    return {};
#else
    if (!Init()) return {};
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return {};
    std::copy(path.begin(), path.end(), addr.sun_path);
    Socket s(socket(AF_UNIX, SOCK_STREAM, 0));
    if (!s.Valid()) return {};
    if (connect(s.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return {};
    return s;
#endif
}

}  // namespace tethercam::net
