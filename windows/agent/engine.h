#pragma once

// The agent's worker: finds the iPhone, runs the protocol session, decodes video and feeds the
// virtual camera through shared memory. See docs/DESIGN.md "Windows agent internals".

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tethercam/formats.h"
#include "tethercam/net.h"
#include "tethercam/protocol.h"
#include "tethercam/shm.h"

namespace tethercam::agent {

struct Snapshot {
    shm::AgentStatus status = shm::AgentStatus::NoDevice;
    std::string device_name;
    std::string detail;  // e.g. "1920x1080 @ 30 fps, 11.8 Mbps"
    std::vector<proto::Camera> cameras;
    int selected_camera = -1;
    bool mirror = false;
    uint16_t rotation = 0;
};

class Engine {
public:
    struct Options {
        std::string tcp;  // "host:port": use plain TCP (fakephone) instead of usbmuxd
        bool prefer_hardware_decode = true;
    };

    explicit Engine(Options options) : opt_(std::move(options)) {}
    ~Engine() { StopThread(); }

    void Start(std::function<void()> on_change);
    void StopThread();

    Snapshot Get() const;
    void SelectCamera(int camera_id);
    void SetMirror(bool mirror);
    void SetRotation(uint16_t degrees);

private:
    struct Consumer {
        bool active = false;
        MediaFormat format;
    };

    void Run();
    bool OpenLink(net::Socket& out);
    void RunSession(net::Socket& sock);
    Consumer TickShm();
    void SetStatus(shm::AgentStatus s, const std::string& detail = {});
    void Notify();

    Options opt_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<net::Socket*> live_socket_{nullptr};
    std::function<void()> on_change_;

    mutable std::mutex mu_;
    Snapshot snap_;
    std::atomic<bool> options_dirty_{false};
    std::atomic<bool> camera_dirty_{false};

    shm::Objects shm_;
    bool shm_open_ = false;
    uint64_t last_shm_try_ = 0;
    unsigned long last_shm_error_ = 0;
    std::string last_vcam_message_;
};

}  // namespace tethercam::agent
