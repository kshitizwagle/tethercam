// fakephone: plays the device side of the Tethercam protocol over plain TCP, streaming an
// H.264-encoded test pattern. Lets the agent and virtual camera be developed without an iPhone.
//
//   fakephone [--port 5321] [--max WxH]
//
// Builds on Windows (Media Foundation encoder) and Linux (FFmpeg encoder, linux/agent/encoder.h).
//
// Connect with:  tethercam-agent --tcp 127.0.0.1:5321   or   tcprobe stream --tcp 127.0.0.1:5321

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "tethercam/annexb.h"
#include "tethercam/formats.h"
#include "tethercam/image.h"
#include "tethercam/log.h"
#ifdef _WIN32
#include "tethercam/media.h"
#else
#include "encoder.h"  // linux/agent: FFmpeg encoder with the same interface
#endif
#include "tethercam/net.h"
#include "tethercam/protocol.h"
#include "tethercam/version.h"

using namespace tethercam;
using Clock = std::chrono::steady_clock;

namespace {

uint64_t NowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

proto::Caps MakeCaps(uint32_t max_w, uint32_t max_h) {
    proto::Caps caps;
    proto::Camera cam;
    cam.camera_id = 0;
    cam.position = proto::Position::Back;
    cam.lens = proto::Lens::Wide;
    cam.label = "Fake Back Camera";
    const proto::Format all[] = {{640, 480, 60}, {1280, 720, 60}, {1920, 1080, 60}, {2560, 1440, 30}, {3840, 2160, 30}};
    for (const auto& f : all)
        if (f.width <= max_w && f.height <= max_h) cam.formats.push_back(f);
    caps.cameras.push_back(cam);
    proto::Camera front = cam;
    front.camera_id = 1;
    front.position = proto::Position::Front;
    front.lens = proto::Lens::Front;
    front.label = "Fake Front Camera";
    front.formats = {{1280, 720, 30}, {1920, 1080, 30}};
    caps.cameras.push_back(front);
    caps.codecs = 1;
    return caps;
}

class Session {
public:
    Session(net::Socket s, proto::Caps caps) : sock_(std::move(s)), caps_(std::move(caps)) {}

    void Run() {
        proto::Hello hello;
        hello.role = proto::Role::Device;
        hello.app_version = std::string(Version());
        hello.name = "fakephone";
        hello.model = "Simulator";
        if (!Send(hello) || !Send(caps_)) return;

        std::vector<uint8_t> buf(64 * 1024);
        auto next_frame = Clock::now();
        auto last_beat = Clock::now();
        auto last_status = Clock::now();
        uint64_t window_bytes = 0, window_frames = 0;
        for (;;) {
            int timeout = 1000;
            if (streaming_) {
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - Clock::now());
                timeout = static_cast<int>(std::max<int64_t>(0, wait.count()));
            }
            const int r = sock_.WaitReadable(timeout);
            if (r < 0) return;
            if (r > 0) {
                const int n = sock_.RecvSome(buf);
                if (n <= 0) {
                    log::Info("host disconnected");
                    return;
                }
                parser_.Feed({buf.data(), size_t(n)});
                proto::Message m;
                for (;;) {
                    auto res = parser_.Next(m);
                    if (res == proto::StreamParser::Result::NeedMore) break;
                    if (res == proto::StreamParser::Result::Fatal) {
                        log::Error("corrupt stream from host");
                        return;
                    }
                    if (!Handle(m)) return;
                }
            }
            const auto now = Clock::now();
            if (streaming_ && now >= next_frame) {
                const size_t sent = SendFrame();
                if (sent == SIZE_MAX) return;
                window_bytes += sent;
                ++window_frames;
                next_frame += std::chrono::microseconds(1'000'000 / fmt_.fps);
                if (now - next_frame > std::chrono::milliseconds(200)) next_frame = now;  // fell behind
            }
            if (now - last_beat >= std::chrono::seconds(1)) {
                last_beat = now;
                if (!Send(proto::Heartbeat{NowUs()})) return;
            }
            if (streaming_ && now - last_status >= std::chrono::seconds(2)) {
                const double dt = std::chrono::duration<double>(now - last_status).count();
                last_status = now;
                proto::Status st;
                st.battery_percent = 100;
                st.charging = 1;
                st.actual_bitrate_kbps = static_cast<uint32_t>(window_bytes * 8 / dt / 1000);
                st.actual_fps_x100 = static_cast<uint16_t>(window_frames * 100 / dt);
                window_bytes = window_frames = 0;
                if (!Send(st)) return;
            }
        }
    }

private:
    bool Send(const proto::Message& m) { return sock_.SendAll(proto::Encode(m)); }

    bool Handle(const proto::Message& m) {
        if (auto h = std::get_if<proto::Hello>(&m)) {
            log::Info("host: {} {} ({})", h->name, h->app_version, h->model);
        } else if (auto c = std::get_if<proto::SetCamera>(&m)) {
            camera_ = c->camera_id;
            log::Info("camera -> {}", camera_);
            if (streaming_) need_config_ = true;
        } else if (auto f = std::get_if<proto::SetFormat>(&m)) {
            fmt_ = {f->width, f->height, std::max<uint32_t>(1, f->fps)};
            bitrate_ = f->bitrate_kbps ? f->bitrate_kbps : DefaultBitrateKbps(fmt_.width, fmt_.height, fmt_.fps);
            log::Info("format -> {}x{} @ {}, {} kbps", fmt_.width, fmt_.height, fmt_.fps, bitrate_);
            need_config_ = true;
        } else if (std::holds_alternative<proto::Start>(m)) {
            log::Info("start");
            streaming_ = true;
            need_config_ = true;
        } else if (std::holds_alternative<proto::Stop>(m)) {
            log::Info("stop");
            streaming_ = false;
        } else if (std::holds_alternative<proto::RequestKeyframe>(m)) {
            force_key_ = true;
        } else if (auto o = std::get_if<proto::SetOptions>(&m)) {
            log::Info("options: mirror={} rotation={}", o->mirror, o->rotation);
        }
        return true;
    }

    // Returns bytes sent, or SIZE_MAX on socket error.
    size_t SendFrame() {
        if (need_config_) {
            if (fmt_.width == 0) fmt_ = {1920, 1080, 30};
            if (!bitrate_) bitrate_ = DefaultBitrateKbps(fmt_.width, fmt_.height, fmt_.fps);
            if (!encoder_.Init(fmt_.width, fmt_.height, fmt_.fps, bitrate_)) {
                log::Error("encoder init failed for {}x{}", fmt_.width, fmt_.height);
                Send(proto::Error{proto::ErrorCode::EncoderFailure, "encoder init failed"});
                streaming_ = false;
                return 0;
            }
            frame_.Resize(fmt_.width, fmt_.height);
            need_config_ = false;
            config_sent_ = false;
            force_key_ = true;
        }
        image::TestPattern(frame_, frame_number_ + camera_ * 1000);
        const uint64_t pts = NowUs();
        std::vector<std::vector<uint8_t>> aus;
        encoder_.Encode(frame_, int64_t(pts * 10), force_key_, aus);
        force_key_ = false;
        ++frame_number_;
        size_t sent = 0;
        for (auto& au : aus) {
            const bool key = annexb::HasIdr(au);
            if (key) {
                auto ps = annexb::ExtractParameterSets(au);
                if (!ps.empty() && (!config_sent_ || ps != last_ps_)) {
                    proto::VideoConfig vc;
                    vc.width = static_cast<uint16_t>(fmt_.width);
                    vc.height = static_cast<uint16_t>(fmt_.height);
                    vc.fps = static_cast<uint16_t>(fmt_.fps);
                    vc.parameter_sets = ps;
                    if (!Send(vc)) return SIZE_MAX;
                    last_ps_ = std::move(ps);
                    config_sent_ = true;
                }
            }
            if (!config_sent_) continue;  // wait for the first IDR
            proto::VideoFrame vf;
            vf.pts_us = pts;
            vf.frame_flags = key ? 1 : 0;
            vf.data = annexb::StripParameterSets(au);
            sent += vf.data.size();
            if (!Send(vf)) return SIZE_MAX;
        }
        return sent;
    }

    net::Socket sock_;
    proto::Caps caps_;
    proto::StreamParser parser_;
    media::H264Encoder encoder_;
    image::Nv12Frame frame_;
    MediaFormat fmt_{1920, 1080, 30};
    uint32_t bitrate_ = 0;
    uint8_t camera_ = 0;
    uint64_t frame_number_ = 0;
    bool streaming_ = false;
    bool need_config_ = true;
    bool config_sent_ = false;
    bool force_key_ = true;
    std::vector<uint8_t> last_ps_;
};

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = proto::kDevicePort;
    uint32_t max_w = 3840, max_h = 2160;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (k == "--max" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &max_w, &max_h);
    }
#ifdef _WIN32
    media::MfScope mf;
    if (!mf.Ok()) return std::fprintf(stderr, "Media Foundation unavailable\n"), 1;
#endif
    auto listener = net::ListenTcp("127.0.0.1", port);
    if (!listener.Valid()) return std::fprintf(stderr, "cannot listen on 127.0.0.1:%u\n", port), 1;
    log::Info("fakephone {} listening on 127.0.0.1:{}", Version(), port);
    const auto caps = MakeCaps(max_w, max_h);
    for (;;) {
        auto client = net::Accept(listener);
        if (!client.Valid()) continue;
        client.SetNoDelay();
        log::Info("host connected");
        Session(std::move(client), caps).Run();
    }
}
