#pragma once

// Local status API (docs/STATUS_API.md): lets stream overlays (browser sources) react to the camera
// state, e.g. show their own "camera offline" design instead of a black frame.
//
//   GET http://127.0.0.1:4790/status   -> current status as JSON
//   GET http://127.0.0.1:4790/events   -> Server-Sent Events: the status JSON on every change
//
// Loopback only, read-only, CORS open (overlays are served from other local ports).

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tethercam/net.h"

namespace tethercam::agent {

class StatusServer {
public:
    static constexpr uint16_t kDefaultPort = 4790;

    explicit StatusServer(std::function<std::string()> status_json) : status_json_(std::move(status_json)) {}
    ~StatusServer() { Stop(); }

    bool Start(uint16_t port = kDefaultPort);
    void Stop();
    // Pushes the current status to every /events subscriber if it changed. Thread-safe.
    void Broadcast();

private:
    void AcceptLoop();
    void Serve(net::Socket client);
    void KeepAliveLoop();

    std::function<std::string()> status_json_;
    net::Socket listener_;
    std::thread accept_thread_, keepalive_thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<net::Socket> subscribers_;
    std::string last_sent_;
};

}  // namespace tethercam::agent
