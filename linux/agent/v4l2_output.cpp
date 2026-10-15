#include "v4l2_output.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "tethercam/log.h"

namespace fs = std::filesystem;

namespace tethercam::agent {
namespace {

std::string ReadLine(const fs::path& p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}

bool IsLoopback(const fs::path& sysdir) {
    std::error_code ec;
    // v4l2loopback adds its own sysfs attributes (0.15 creates virtual devices with no parent).
    if (fs::exists(sysdir / "max_openers", ec)) return true;
    // Older versions: a platform device named v4l2loopback.N.
    const auto target = fs::read_symlink(sysdir / "device", ec);
    return !ec && target.filename().string().starts_with("v4l2loopback");
}

}  // namespace

std::string FindLoopbackDevice() {
    std::error_code ec;
    std::string first;
    for (const auto& e : fs::directory_iterator("/sys/class/video4linux", ec)) {
        if (!IsLoopback(e.path())) continue;
        const std::string dev = "/dev/" + e.path().filename().string();
        if (ReadLine(e.path() / "name") == "Tethercam") return dev;
        if (first.empty() || dev < first) first = dev;
    }
    return first;
}

int CountOtherOpeners(const std::string& device) {
    std::error_code ec;
    const fs::path target = fs::weakly_canonical(device, ec);
    const std::string self = std::to_string(getpid());
    int count = 0;
    for (const auto& proc : fs::directory_iterator("/proc", ec)) {
        const std::string pid = proc.path().filename().string();
        if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0])) || pid == self) continue;
        std::error_code fec;
        for (const auto& fd : fs::directory_iterator(proc.path() / "fd", fec)) {
            std::error_code lec;
            if (fs::read_symlink(fd.path(), lec) == target && !lec) {
                ++count;
                break;
            }
        }
    }
    return count;
}

bool V4l2Output::Open(const std::string& device, const MediaFormat& format, PixelFormat pixel_format) {
    if (fd_ < 0 || device != device_) {
        Close();
        fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            // Retried every second; only log when the reason changes.
            std::string err = std::format("cannot open {}: {}", device, std::strerror(errno));
            if (err != last_error_) log::Warn("{}", err);
            last_error_ = std::move(err);
            return false;
        }
        last_error_.clear();
        device_ = device;
    }
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f.fmt.pix.width = format.width;
    f.fmt.pix.height = format.height;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;  // BT.601 limited range, like the Windows camera
    if (pixel_format == PixelFormat::Yuyv) {
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        f.fmt.pix.bytesperline = format.width * 2;
        f.fmt.pix.sizeimage = format.width * format.height * 2;
    } else {
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        f.fmt.pix.bytesperline = format.width;
        f.fmt.pix.sizeimage = format.width * format.height * 3 / 2;
    }
    if (ioctl(fd_, VIDIOC_S_FMT, &f) < 0) {
        log::Warn("{}: cannot set {}x{} ({})", device, format.width, format.height, std::strerror(errno));
        return false;
    }
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.timeperframe = {1, format.fps};
    ioctl(fd_, VIDIOC_S_PARM, &parm);  // advisory: readers see this rate
    format_ = format;
    pixel_format_ = pixel_format;
    out_.resize(f.fmt.pix.sizeimage);
    black_.Resize(format.width, format.height);
    image::FillBlack(black_);
    log::Info("virtual camera {}: {}x{} @ {} {}", device, format.width, format.height, format.fps,
              pixel_format == PixelFormat::Yuyv ? "YUYV" : "NV12");
    return true;
}

void V4l2Output::Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool V4l2Output::Write(const image::Nv12View& frame) {
    if (fd_ < 0) return false;
    if (frame.width == format_.width && frame.height == format_.height) return WriteNv12(frame);
    scaled_.Resize(format_.width, format_.height);
    image::ScaleLetterbox(frame, scaled_);
    return WriteNv12(scaled_.View());
}

bool V4l2Output::WriteBlack() { return fd_ >= 0 && WriteNv12(black_.View()); }

bool V4l2Output::WriteNv12(const image::Nv12View& v) {
    const uint32_t w = format_.width, h = format_.height;
    if (pixel_format_ == PixelFormat::Nv12) {
        for (uint32_t y = 0; y < h; ++y) std::memcpy(out_.data() + size_t(y) * w, v.y + size_t(y) * v.y_pitch, w);
        uint8_t* uv = out_.data() + size_t(w) * h;
        for (uint32_t y = 0; y < h / 2; ++y) std::memcpy(uv + size_t(y) * w, v.uv + size_t(y) * v.uv_pitch, w);
    } else {
        // YUYV: Y0 U Y1 V per pixel pair; chroma rows are shared by two luma rows.
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* yr = v.y + size_t(y) * v.y_pitch;
            const uint8_t* uvr = v.uv + size_t(y / 2) * v.uv_pitch;
            uint8_t* o = out_.data() + size_t(y) * w * 2;
            for (uint32_t x = 0; x < w; x += 2) {
                o[0] = yr[x];
                o[1] = uvr[x];
                o[2] = yr[x + 1];
                o[3] = uvr[x + 1];
                o += 4;
            }
        }
    }
    const ssize_t n = ::write(fd_, out_.data(), out_.size());
    return n == ssize_t(out_.size());
}

}  // namespace tethercam::agent
