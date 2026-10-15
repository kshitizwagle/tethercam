#include "frame_source.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "tethercam/media.h"

namespace tethercam::vcam {
namespace {

constexpr uint64_t kAgentTimeoutMs = 1500;  // agent heartbeat older than this = agent gone
constexpr uint64_t kFrameTimeoutMs = 750;   // no new frame for this long = show placeholder

void CopyPlanes(const image::Nv12Frame& src, uint8_t* y, uint32_t y_pitch, uint8_t* uv, uint32_t uv_pitch) {
    const uint32_t w = src.width, h = src.height;
    const uint8_t* sy = src.data.data();
    const uint8_t* suv = sy + size_t(w) * h;
    for (uint32_t r = 0; r < h; ++r) std::memcpy(y + size_t(r) * y_pitch, sy + size_t(r) * w, w);
    for (uint32_t r = 0; r < h / 2; ++r) std::memcpy(uv + size_t(r) * uv_pitch, suv + size_t(r) * w, w);
}

}  // namespace

bool FrameSource::EnsureShm() {
    if (shm_ok_) return true;
    const uint64_t now = GetTickCount64();
    if (now - last_shm_attempt_ < 1000) return false;
    last_shm_attempt_ = now;
    shm_ok_ = shm::Create(shm_) || shm::Open(shm_);
    if (shm_ok_) Report("vcam: shared memory ready");
    else {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "Tethercam vcam: shared memory unavailable (error %lu)\n", GetLastError());
        OutputDebugStringA(msg);
    }
    return shm_ok_;
}

void FrameSource::Report(const char* message) {
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    if (!shm_ok_) return;
    auto* h = static_cast<shm::Header*>(shm_.view);
    strncpy_s(h->vcam_message, message, _TRUNCATE);
}

void FrameSource::Start(uint32_t width, uint32_t height, uint32_t fps) {
    std::lock_guard lock(mu_);
    width_ = width;
    height_ = height;
    fps_ = fps ? fps : 30;
    placeholder_status_ = shm::AgentStatus(0xFFFFFFFF);
    last_new_frame_ms_ = 0;
    if (EnsureShm()) Announce("started");
}

void FrameSource::Announce(const char* verb) {
    auto* h = static_cast<shm::Header*>(shm_.view);
    h->req_width = width_;
    h->req_height = height_;
    h->req_fps = fps_;
    shm::Atomic(h->consumer_active).store(1, std::memory_order_release);
    shm::Atomic(h->control_seq).fetch_add(1, std::memory_order_acq_rel);
    SetEvent(shm_.control);
    char msg[96];
    std::snprintf(msg, sizeof(msg), "vcam: %s %ux%u @ %u", verb, width_, height_, fps_);
    Report(msg);
}

void FrameSource::Stop() {
    std::lock_guard lock(mu_);
    if (!shm_ok_) return;
    auto* h = static_cast<shm::Header*>(shm_.view);
    shm::Atomic(h->consumer_active).store(0, std::memory_order_release);
    shm::Atomic(h->control_seq).fetch_add(1, std::memory_order_acq_rel);
    SetEvent(shm_.control);
    Report("vcam: stopped");
}

const image::Nv12Frame& FrameSource::Placeholder(shm::AgentStatus status) {
    if (placeholder_.width == width_ && placeholder_.height == height_ && placeholder_status_ == status)
        return placeholder_;
    placeholder_.Resize(width_, height_);
    std::wstring title, detail;
    switch (status) {
    case shm::AgentStatus::NoDevice:
        title = L"Connect your iPhone";
        detail = L"Plug in your iPhone with a USB cable and open the Tethercam app.";
        break;
    case shm::AgentStatus::AppNotOpen:
        title = L"Open Tethercam on your iPhone";
        detail = L"Unlock your iPhone, open the Tethercam app, and tap Trust if asked.";
        break;
    case shm::AgentStatus::Connected:
    case shm::AgentStatus::Streaming:
        title = L"Starting camera…";
        detail = L"Waiting for video from your iPhone.";
        break;
    case shm::AgentStatus::PausedOnPhone:
        title = L"Paused on iPhone";
        detail = L"Bring the Tethercam app back to the front and keep the iPhone unlocked.";
        break;
    case shm::AgentStatus::NoUsbmuxd:
        title = L"Apple Devices is required";
        detail = L"Install the Apple Devices app from the Microsoft Store (or iTunes) so Windows can talk to your iPhone.";
        break;
    default:
        title = L"Tethercam";
        detail = L"The Tethercam agent is not running on this PC. Start Tethercam from the Start menu.";
        break;
    }
    media::RenderPlaceholder(placeholder_, title, detail);
    placeholder_status_ = status;
    return placeholder_;
}

FillResult FrameSource::Fill(uint8_t* y, uint32_t y_pitch, uint8_t* uv, uint32_t uv_pitch, uint32_t grace_ms) {
    std::lock_guard lock(mu_);
    FillResult out;
    if (width_ == 0) return out;
    shm::AgentStatus status = shm::AgentStatus::None;
    bool status_card = false;  // default: plain black while not live, so stream layouts show through cleanly
    if (EnsureShm()) {
        auto* h = static_cast<shm::Header*>(shm_.view);
        // consumer_active is one flag shared by every source instance. When an app swaps one
        // instance for another (OBS changing scenes), the old one's Stop can land after the new
        // one's Start; a running stream re-asserts the flag so the agent keeps the phone streaming.
        if (shm::Atomic(h->consumer_active).load(std::memory_order_acquire) == 0) Announce("resumed");
        const uint64_t now = GetTickCount64();
        const uint64_t beat = shm::Atomic(h->agent_heartbeat_ms).load(std::memory_order_acquire);
        const bool agent_alive = beat != 0 && now >= beat && now - beat < kAgentTimeoutMs;
        if (agent_alive) {
            status = static_cast<shm::AgentStatus>(shm::Atomic(h->agent_status).load(std::memory_order_acquire));
            status_card = shm::Atomic(h->placeholder_mode).load(std::memory_order_relaxed) == 1;

            // The agent signals FrameReady on every publish. If the newest frame is one we have
            // already delivered, give a late frame a short window before repeating it: the sample
            // still goes out on time, but a frame landing mid-wait is shown fresh.
            shm::FrameInfo peek;
            if (grace_ms && shm_.frame_ready && shm::PeekLatest(shm_.view, peek) &&
                peek.frame_number == last_frame_number_ && peek.width == width_ && peek.height == height_)
                WaitForSingleObject(shm_.frame_ready, grace_ms);

            shm::FrameInfo info;
            if (shm::ReadLatest(shm_.view, width_, height_, y, y_pitch, info)) {
                out.fresh = info.frame_number != last_frame_number_;
                if (out.fresh) {
                    last_frame_number_ = info.frame_number;
                    last_new_frame_ms_ = now;
                }
                if (now - last_new_frame_ms_ < kFrameTimeoutMs) {
                    out.live = true;
                    out.pts_100ns = info.pts_100ns;
                    return out;
                }
            }
        }
    }
    if (status_card) {
        CopyPlanes(Placeholder(status), y, y_pitch, uv, uv_pitch);
    } else {
        for (uint32_t r = 0; r < height_; ++r) std::memset(y + size_t(r) * y_pitch, 16, width_);
        for (uint32_t r = 0; r < height_ / 2; ++r) std::memset(uv + size_t(r) * uv_pitch, 128, width_);
    }
    return out;
}

}  // namespace tethercam::vcam
