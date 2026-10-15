#pragma once

// Supplies NV12 frames to the media stream: live frames from the agent through shared memory,
// or a placeholder when no live frame is available. Never blocks for long.

#include <cstdint>
#include <mutex>

#include "tethercam/image.h"
#include "tethercam/shm.h"

namespace tethercam::vcam {

class FrameSource {
public:
    // What Fill wrote: a live frame (live; fresh = new since the last call), another copy of the
    // last live frame (live, !fresh — repeat instead of freeze), or placeholder/black (!live).
    struct FillResult {
        bool live = false;
        bool fresh = false;
        uint64_t pts_100ns = 0;  // capture timestamp of the live frame
    };

    // Publishes the requested format to the agent and marks a consumer as active.
    void Start(uint32_t width, uint32_t height, uint32_t fps);
    void Stop();

    // Writes one frame of the started size into dst (NV12 with the given pitch; UV plane at uv).
    // When the newest frame was already delivered, waits up to `grace_ms` for the agent's FrameReady
    // signal (a frame landing just after the due time is used instead of repeating).
    FillResult Fill(uint8_t* y, uint32_t y_pitch, uint8_t* uv, uint32_t uv_pitch, uint32_t grace_ms = 0);

    // Records a status line in shared memory for the agent's log.
    void Report(const char* message);

private:
    bool EnsureShm();
    // Publishes the started format and sets consumer_active. Call with mu_ held and shm ready.
    void Announce(const char* verb);
    const image::Nv12Frame& Placeholder(shm::AgentStatus status);

    std::mutex mu_;
    shm::Objects shm_;
    bool shm_ok_ = false;
    uint64_t last_shm_attempt_ = 0;
    uint32_t width_ = 0, height_ = 0, fps_ = 30;
    image::Nv12Frame placeholder_;
    shm::AgentStatus placeholder_status_ = shm::AgentStatus(0xFFFFFFFF);
    uint64_t last_frame_number_ = 0;
    uint64_t last_new_frame_ms_ = 0;
};

}  // namespace tethercam::vcam
