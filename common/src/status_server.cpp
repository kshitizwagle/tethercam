#include "tethercam/status_server.h"

#include <chrono>
#include <span>

#include "tethercam/log.h"

namespace tethercam::agent {
namespace {

constexpr char kCors[] =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
    "Access-Control-Allow-Private-Network: true\r\n";

bool SendText(net::Socket& s, const std::string& text) {
    return s.SendAll(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::string Response(const char* status, const char* type, const std::string& body) {
    return std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n" + kCors + "\r\n" +
           body;
}

}  // namespace

bool StatusServer::Start(uint16_t port) {
    listener_ = net::ListenTcp("127.0.0.1", port);
    if (!listener_.Valid()) {
        log::Warn("status API: cannot listen on 127.0.0.1:{}", port);
        return false;
    }
    stop_ = false;
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    keepalive_thread_ = std::thread([this] { KeepAliveLoop(); });
    log::Info("status API on http://127.0.0.1:{}/status and /events", port);
    return true;
}

void StatusServer::Stop() {
    stop_ = true;
    listener_.Shutdown();
    listener_.Close();
    {
        std::lock_guard lock(mu_);
        for (auto& s : subscribers_) s.Shutdown();
        subscribers_.clear();
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    if (keepalive_thread_.joinable()) keepalive_thread_.join();
}

void StatusServer::AcceptLoop() {
    while (!stop_) {
        auto client = net::Accept(listener_);
        if (!client.Valid()) {
            if (stop_) return;
            continue;
        }
        Serve(std::move(client));
    }
}

void StatusServer::Serve(net::Socket client) {
    // Read the request head (small; we only need the request line).
    std::string req;
    uint8_t buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        if (client.WaitReadable(2000) <= 0) return;
        const int n = client.RecvSome(buf);
        if (n <= 0) return;
        req.append(reinterpret_cast<char*>(buf), size_t(n));
    }
    const auto sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    const std::string method = req.substr(0, sp1);
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    path = path.substr(0, path.find('?'));

    if (method == "OPTIONS") {
        SendText(client, Response("204 No Content", "text/plain", ""));
    } else if (method != "GET") {
        SendText(client, Response("405 Method Not Allowed", "text/plain", "GET only\n"));
    } else if (path == "/status" || path == "/") {
        SendText(client, Response("200 OK", "application/json", status_json_() + "\n"));
    } else if (path == "/events") {
        const std::string head = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                             "Cache-Control: no-cache\r\nConnection: keep-alive\r\n") +
                                 kCors + "\r\nretry: 1000\n\n";
        const std::string first = "data: " + status_json_() + "\n\n";
        if (!SendText(client, head) || !SendText(client, first)) return;
        std::lock_guard lock(mu_);
        subscribers_.push_back(std::move(client));  // kept open; Broadcast() writes to it
    } else {
        SendText(client, Response("404 Not Found", "text/plain", "try /status or /events\n"));
    }
}

void StatusServer::Broadcast() {
    const std::string json = status_json_();
    std::lock_guard lock(mu_);
    if (json == last_sent_) return;
    last_sent_ = json;
    const std::string event = "data: " + json + "\n\n";
    std::erase_if(subscribers_, [&](net::Socket& s) { return !SendText(s, event); });
}

void StatusServer::KeepAliveLoop() {
    // SSE comment every 15 s keeps idle connections open and drops dead subscribers.
    while (!stop_) {
        for (int i = 0; i < 150 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lock(mu_);
        std::erase_if(subscribers_, [](net::Socket& s) { return !SendText(s, ": keep-alive\n\n"); });
    }
}

}  // namespace tethercam::agent
