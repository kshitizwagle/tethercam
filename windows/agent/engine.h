#pragma once

// The agent's worker: finds the iPhone, runs the protocol session, decodes video and feeds the
// virtual camera through shared memory. See docs/DESIGN.md "Windows agent internals".

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tethercam/agent_state.h"
#include "tethercam/discovery.h"
#include "tethercam/formats.h"
#include "tethercam/image.h"
#include "tethercam/link.h"
#include "tethercam/net.h"
#include "tethercam/pairing.h"
#include "tethercam/protocol.h"
#include "tethercam/shm.h"

namespace tethercam::agent {

class Engine {
public:
    struct Options {
        std::string tcp;   // "host:port": use plain TCP (fakephone) instead of usbmuxd; pairs like USB
        std::string wifi;  // "host[:port]": connect over Wi-Fi to the paired phone at this address
        // Wi-Fi pairing file; empty = the per-user default, or memory only when persist_settings is off.
        std::string pairing_file;
        bool prefer_hardware_decode = true;
        // Isolated protocol tests and embedding: do not touch the user's registry or camera mapping.
        bool persist_settings = true;
        bool shared_memory = true;
    };

    explicit Engine(Options options) : opt_(std::move(options)) {}
    ~Engine() { StopThread(); }

    void Start(std::function<void()> on_change);
    void StopThread();

    Snapshot Get() const;
    void SelectCamera(int camera_id);
    void SetMirror(bool mirror);
    void SetRotation(uint16_t degrees);
    void SetStatusCard(bool on);
    void SetQuality(proto::Quality quality);
    // Forgets every Wi-Fi pairing; the next USB connection pairs again if Wireless is on.
    void ForgetWifi();
    // Address ("host[:port]") to try first for the paired phone; empty clears it.
    void SetWifiAddress(const std::string& address);

    // Machine-readable status for the local status API (docs/STATUS_API.md).
    std::string StatusJson() const;

    // The preview window counts as a consumer: while it's open the phone streams even if no app
    // has the virtual camera open.
    void SetPreview(bool active) { preview_active_ = active; }
    // Copies the latest preview frame (at most 1280x720) if it's newer than `seq`. Thread-safe.
    bool CopyPreview(image::Nv12Frame& out, uint64_t& seq) const;

private:
    struct Consumer {
        bool active = false;
        MediaFormat format;
    };

    void Run();
    bool OpenLink(std::unique_ptr<Link>& out);
    bool OpenWifi(std::unique_ptr<Link>& out);
    void RunSession(Link& link);
    void UpdateWifiSnapshotLocked();  // caller holds mu_
    void SavePairingLocked();         // caller holds mu_
    Consumer TickShm();
    void SetStatus(shm::AgentStatus s, const std::string& detail = {});
    void RememberCameraLocked();  // persists the selected lens's label; caller holds mu_
    void Notify();
    std::string ReadSetting(const wchar_t* name) const;
    void WriteSetting(const wchar_t* name, const std::string& value) const;

    Options opt_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<Link*> live_link_{nullptr};
    std::atomic<bool> wifi_session_{false};
    std::function<void()> on_change_;

    mutable std::mutex mu_;
    Snapshot snap_;
    std::string preferred_camera_;  // label of the last lens the user picked (PC or phone)
    std::atomic<bool> options_dirty_{false};
    std::atomic<bool> camera_dirty_{false};
    std::atomic<bool> quality_dirty_{false};

    wifi::PairingStore pairing_;                  // guarded by mu_
    std::unique_ptr<wifi::Discovery> discovery_;  // started once a phone is paired
    std::string link_serial_;                     // USB serial of the current plain link
    std::chrono::steady_clock::time_point last_wifi_try_{};
    std::string last_wifi_warning_;

    shm::Objects shm_;
    bool shm_open_ = false;
    uint64_t last_shm_try_ = 0;
    unsigned long last_shm_error_ = 0;
    std::string last_vcam_message_;

    std::atomic<bool> preview_active_{false};
    mutable std::mutex preview_mu_;
    image::Nv12Frame preview_;
    uint64_t preview_seq_ = 0;
};

}  // namespace tethercam::agent
