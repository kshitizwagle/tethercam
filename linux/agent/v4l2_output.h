#pragma once

// The Linux virtual camera: frames written to a v4l2loopback device (docs/TECH_SPEC.md §13).
// v4l2loopback has no per-app format negotiation, so the agent picks one output format (R9).

#include <cstdint>
#include <string>
#include <vector>

#include "tethercam/formats.h"
#include "tethercam/image.h"

namespace tethercam::agent {

enum class PixelFormat { Yuyv, Nv12 };

// Finds the loopback device: the one labelled "Tethercam", else the first v4l2loopback device.
// Empty if there is none (module not loaded).
std::string FindLoopbackDevice();

// Counts other processes that have `device` open (readers of the camera). Only sees processes
// of the same user, which is where desktop apps run.
int CountOtherOpeners(const std::string& device);

class V4l2Output {
public:
    V4l2Output() = default;
    ~V4l2Output() { Close(); }
    V4l2Output(const V4l2Output&) = delete;
    V4l2Output& operator=(const V4l2Output&) = delete;

    // Opens `device` and sets the output format. Can be called again to change the format; that
    // only works while no app reads the camera.
    bool Open(const std::string& device, const MediaFormat& format, PixelFormat pixel_format);
    void Close();
    bool IsOpen() const { return fd_ >= 0; }
    const std::string& Device() const { return device_; }
    const MediaFormat& Format() const { return format_; }

    // Converts (and scales/letterboxes, if the size differs) one NV12 frame and writes it.
    bool Write(const image::Nv12View& frame);
    // Writes a black frame, so readers keep getting frames while there's no video.
    bool WriteBlack();
    // Re-sends the last written frame: a brief stall holds the picture instead of flashing black.
    bool Repeat();

private:
    bool WriteNv12(const image::Nv12View& v);
    bool RawWrite();

    int fd_ = -1;
    std::string device_;
    MediaFormat format_;
    PixelFormat pixel_format_ = PixelFormat::Yuyv;
    image::Nv12Frame scaled_;
    image::Nv12Frame black_;
    std::vector<uint8_t> out_;
    bool has_frame_ = false;  // out_ holds a frame ready to repeat
    std::string last_error_;
};

}  // namespace tethercam::agent
