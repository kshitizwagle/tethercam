#pragma once

// The Linux agent's worker: finds the iPhone, runs the protocol session, decodes video and feeds the
// v4l2loopback virtual camera. Mirrors windows/agent/engine.h; see docs/TECH_SPEC.md §13.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "settings.h"
#include "tethercam/agent_state.h"
#include "tethercam/discovery.h"
#include "tethercam/formats.h"
#include "tethercam/image.h"
#include "tethercam/link.h"
#include "tethercam/net.h"
#include "tethercam/pairing.h"
#include "tethercam/protocol.h"
#include "v4l2_output.h"

namespace tethercam::agent {

class Engine {
public:
    struct Options {
        std::string tcp;     // "host:port": use plain TCP (fakephone) instead of usbmuxd; pairs like USB
        std::string wifi;    // "host[:port]": connect over Wi-Fi to the paired phone at this address
        std::string pairing_file;  // Wi-Fi pairings; empty = $XDG_CONFIG_HOME/tethercam/pairing
        std::string device;  // v4l2loopback device; empty = find the "Tethercam" one
        PixelFormat pixel_format = PixelFormat::Yuyv;
        bool prefer_hardware_decode = true;
        // Stream even when no app has the camera open (for when opener detection can't see the app).
        bool always_on = false;
    };

    Engine(Options options, Settings& settings) : opt_(std::move(options)), settings_(settings) {}
    ~Engine() { StopThread(); }

    void Start(std::function<void()> on_change);
    void StopThread();

    Snapshot Get() const;
    void SelectCamera(int camera_id);
    void SetMirror(bool mirror);
    void SetRotation(uint16_t degrees);
    void SetQuality(proto::Quality quality);
    // Forgets every Wi-Fi pairing; the next USB connection pairs again if Wireless is on.
    void ForgetWifi();
    // Address ("host[:port]") to try first for the paired phone; empty clears it.
    void SetWifiAddress(const std::string& address);
    std::string StatusJson() const;
    // Path of the virtual camera device, or empty while there's none.
    std::string CameraDevice() const;
    // The preview window counts as a consumer: while it's open the phone streams even if no app
    // has the camera open.
    void SetPreview(bool active) { preview_active_ = active; }
    // Copies the latest preview frame (at most 1280 wide) if it's newer than `seq`. Thread-safe.
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
    // Keeps the camera device open and fed, and reports whether an app is reading it.
    Consumer TickCamera();
    void FeedIdle();
    MediaFormat OutputFormat() const;
    void SetStatus(AgentStatus s, const std::string& detail = {});
    void RememberCameraLocked();
    void Notify();

    Options opt_;
    Settings& settings_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<Link*> live_link_{nullptr};
    std::atomic<bool> wifi_session_{false};
    std::function<void()> on_change_;

    mutable std::mutex mu_;
    Snapshot snap_;
    std::string preferred_camera_;
    std::string camera_device_;
    std::atomic<bool> options_dirty_{false};
    std::atomic<bool> camera_dirty_{false};
    std::atomic<bool> quality_dirty_{false};

    wifi::PairingStore pairing_;                  // guarded by mu_
    std::unique_ptr<wifi::Discovery> discovery_;  // started once a phone is paired
    std::string link_serial_;                     // USB serial of the current plain link
    std::chrono::steady_clock::time_point last_wifi_try_{};
    std::string last_wifi_warning_;

    V4l2Output out_;
    std::chrono::steady_clock::time_point last_write_{}, last_any_write_{}, last_scan_{}, last_open_try_{};
    int openers_ = 0;
    bool warned_no_device_ = false;

    std::atomic<bool> preview_active_{false};
    mutable std::mutex preview_mu_;
    image::Nv12Frame preview_;
    uint64_t preview_seq_ = 0;
};

}  // namespace tethercam::agent
