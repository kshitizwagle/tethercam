// tcprobe: developer tool for the phone link.
//
//   tcprobe list                         list devices known to usbmuxd
//   tcprobe stream [options]             connect, handshake, stream and print stats
//     --tcp HOST:PORT                    connect over plain TCP (e.g. to fakephone) instead of USB
//     --size WxH --fps N --bitrate KBPS  requested format (default 1920x1080 @ 30)
//     --camera ID                        camera id from Caps (default: first)
//     --seconds N                        stop after N seconds (default 10)
//     --dump FILE.h264                   write the Annex-B elementary stream
//     --decode                           decode with Media Foundation and report decoded fps
//     --snapshot FILE.bmp                with --decode: save the last decoded frame
//   tcprobe echo [--mb N] [--port P]     M1 spike: round-trip N MB through an echo server on the phone

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "tethercam/formats.h"
#include "tethercam/media.h"
#include "tethercam/net.h"
#include "tethercam/protocol.h"
#include "tethercam/usbmux.h"
#include "tethercam/version.h"

using namespace tethercam;
using Clock = std::chrono::steady_clock;

namespace {

struct Args {
    std::string command = "stream";
    std::string tcp;
    uint32_t width = 1920, height = 1080, fps = 30, bitrate = 0;
    int camera = -1;
    int seconds = 10;
    int mb = 100;
    uint16_t port = proto::kDevicePort;
    std::string dump;
    bool decode = false;
    std::string snapshot;
};

Args Parse(int argc, char** argv) {
    Args a;
    int i = 1;
    if (argc > 1 && argv[1][0] != '-') a.command = argv[i++];
    for (; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (k == "--tcp") a.tcp = next();
        else if (k == "--size") sscanf_s(next().c_str(), "%ux%u", &a.width, &a.height);
        else if (k == "--fps") a.fps = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (k == "--bitrate") a.bitrate = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (k == "--camera") a.camera = std::atoi(next().c_str());
        else if (k == "--seconds") a.seconds = std::atoi(next().c_str());
        else if (k == "--dump") a.dump = next();
        else if (k == "--decode") a.decode = true;
        else if (k == "--snapshot") a.snapshot = next();
        else if (k == "--mb") a.mb = std::atoi(next().c_str());
        else if (k == "--port") a.port = static_cast<uint16_t>(std::atoi(next().c_str()));
    }
    return a;
}

int List() {
    auto devices = usbmux::Client::ListDevices();
    if (!devices) {
        std::fprintf(stderr, "usbmuxd is not reachable. Install Apple Devices (Microsoft Store) or iTunes.\n");
        return 2;
    }
    std::printf("%zu device(s)\n", devices->size());
    for (const auto& d : *devices)
        std::printf("  id=%u  type=%s  product=0x%04x  serial=%s\n", d.device_id, d.connection_type.c_str(),
                    d.product_id, d.serial.c_str());
    return 0;
}

// Opens a stream to the device: USB through usbmuxd, or plain TCP for fakephone/echo servers.
std::optional<net::Socket> Open(const Args& a) {
    if (!a.tcp.empty()) {
        auto colon = a.tcp.rfind(':');
        auto s = net::ConnectTcp(a.tcp.substr(0, colon), static_cast<uint16_t>(std::atoi(a.tcp.c_str() + colon + 1)));
        if (!s.Valid()) {
            std::fprintf(stderr, "TCP connect to %s failed\n", a.tcp.c_str());
            return std::nullopt;
        }
        s.SetNoDelay();
        return s;
    }
    auto devices = usbmux::Client::ListDevices();
    if (!devices) {
        std::fprintf(stderr, "usbmuxd is not reachable\n");
        return std::nullopt;
    }
    for (const auto& d : *devices) {
        if (d.connection_type != "USB") continue;
        net::Socket s;
        auto r = usbmux::Client::Connect(d.device_id, a.port, s);
        std::printf("connect device %u port %u: %s\n", d.device_id, a.port, usbmux::ToString(r));
        if (r == usbmux::ConnectResult::Ok) return s;
    }
    std::fprintf(stderr, "no USB device accepted the connection\n");
    return std::nullopt;
}

int Echo(const Args& a) {
    auto s = Open(a);
    if (!s) return 2;
    const size_t total = size_t(a.mb) << 20;
    std::vector<uint8_t> out(1 << 16), in(1 << 16);
    std::mt19937 rng(42);
    std::mt19937 check(42);
    size_t sent = 0, recvd = 0;
    const auto t0 = Clock::now();
    // Send in chunks and read back the same amount, so neither side's buffers fill up.
    while (recvd < total) {
        if (sent < total) {
            const size_t n = std::min(out.size(), total - sent);
            for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(rng());
            if (!s->SendAll({out.data(), n})) return std::fprintf(stderr, "send failed\n"), 3;
            sent += n;
        }
        while (recvd < sent) {
            const int n = s->RecvSome({in.data(), std::min(in.size(), sent - recvd)});
            if (n <= 0) return std::fprintf(stderr, "recv failed after %zu bytes\n", recvd), 3;
            for (int i = 0; i < n; ++i)
                if (in[size_t(i)] != static_cast<uint8_t>(check()))
                    return std::fprintf(stderr, "data mismatch at byte %zu\n", recvd + size_t(i)), 4;
            recvd += size_t(n);
        }
    }
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("echoed %d MB intact in %.2f s: %.1f MB/s each way (%.0f Mbps)\n", a.mb, secs, a.mb / secs,
                a.mb * 8.0 * 1.048576 / secs);
    return 0;
}

bool Send(net::Socket& s, const proto::Message& m) { return s.SendAll(proto::Encode(m)); }

int Stream(const Args& a) {
    auto s = Open(a);
    if (!s) return 2;
    proto::Hello hello;
    hello.role = proto::Role::Host;
    hello.app_version = std::string(Version());
    hello.name = "tcprobe";
    hello.model = "Windows";
    Send(*s, hello);

    FILE* dump = nullptr;
    if (!a.dump.empty() && fopen_s(&dump, a.dump.c_str(), "wb") != 0) dump = nullptr;

    proto::StreamParser parser;
    std::vector<uint8_t> buf(1 << 20);
    const auto t0 = Clock::now();
    auto lastReport = t0;
    uint64_t frames = 0, bytes = 0, keyframes = 0, windowFrames = 0, windowBytes = 0;
    bool started = false;
    std::optional<media::MfScope> mf;
    media::H264Decoder decoder;
    bool decoder_ready = false;
    std::vector<uint8_t> parameter_sets;
    uint64_t decoded = 0, window_decoded = 0, decode_errors = 0;
    image::Nv12Frame last;
    if (a.decode) mf.emplace();

    while (Clock::now() - t0 < std::chrono::seconds(a.seconds)) {
        const int w = s->WaitReadable(250);
        if (w < 0) break;
        if (w > 0) {
            const int n = s->RecvSome(buf);
            if (n <= 0) {
                std::printf("connection closed\n");
                break;
            }
            parser.Feed({buf.data(), size_t(n)});
        }
        proto::Message m;
        for (;;) {
            auto r = parser.Next(m);
            if (r == proto::StreamParser::Result::NeedMore) break;
            if (r == proto::StreamParser::Result::Fatal) {
                std::fprintf(stderr, "protocol error: corrupt stream\n");
                return 5;
            }
            if (auto h = std::get_if<proto::Hello>(&m)) {
                std::printf("device: %s (%s), app %s, protocol %u-%u\n", h->name.c_str(), h->model.c_str(),
                            h->app_version.c_str(), h->min_version, h->max_version);
            } else if (auto c = std::get_if<proto::Caps>(&m)) {
                for (const auto& cam : c->cameras) {
                    std::printf("camera %u: %s (%zu formats)\n", cam.camera_id, cam.label.c_str(), cam.formats.size());
                    for (const auto& f : cam.formats) std::printf("    %ux%u @ %u\n", f.width, f.height, f.max_fps);
                }
                if (!started && !c->cameras.empty()) {
                    const auto& cam = a.camera < 0 ? c->cameras[0] : c->cameras[size_t(a.camera) % c->cameras.size()];
                    MediaFormat chosen;
                    ChooseDeviceFormat(cam.formats, {a.width, a.height, a.fps}, chosen);
                    proto::SetFormat sf;
                    sf.width = static_cast<uint16_t>(chosen.width);
                    sf.height = static_cast<uint16_t>(chosen.height);
                    sf.fps = static_cast<uint16_t>(chosen.fps);
                    sf.bitrate_kbps = a.bitrate;
                    std::printf("requesting camera %u %ux%u @ %u\n", cam.camera_id, chosen.width, chosen.height,
                                chosen.fps);
                    Send(*s, proto::SetCamera{cam.camera_id});
                    Send(*s, sf);
                    Send(*s, proto::Start{});
                    started = true;
                }
            } else if (auto vc = std::get_if<proto::VideoConfig>(&m)) {
                std::printf("video config: %ux%u @ %u, %zu bytes of parameter sets\n", vc->width, vc->height, vc->fps,
                            vc->parameter_sets.size());
                if (dump) fwrite(vc->parameter_sets.data(), 1, vc->parameter_sets.size(), dump);
                parameter_sets = vc->parameter_sets;
                if (a.decode) decoder_ready = decoder.Init(vc->width, vc->height, vc->fps, true);
            } else if (auto vf = std::get_if<proto::VideoFrame>(&m)) {
                ++frames;
                ++windowFrames;
                bytes += vf->data.size();
                windowBytes += vf->data.size();
                if (vf->IsKeyframe()) ++keyframes;
                if (dump) fwrite(vf->data.data(), 1, vf->data.size(), dump);
                if (decoder_ready) {
                    std::vector<uint8_t> au;
                    if (vf->IsKeyframe()) au = parameter_sets;
                    au.insert(au.end(), vf->data.begin(), vf->data.end());
                    const bool ok =
                        decoder.Decode(au, int64_t(vf->pts_us * 10), [&](const media::DecodedFrame& f, int64_t) {
                            ++decoded;
                            ++window_decoded;
                            if (!a.snapshot.empty()) {
                                const image::Nv12View v = f.View();
                                if (!v.y) return;
                                last.Resize(v.width, v.height);
                                image::ScaleLetterbox(v, last);
                            }
                        });
                    if (!ok) {
                        ++decode_errors;
                        Send(*s, proto::RequestKeyframe{});
                    }
                }
            } else if (auto st = std::get_if<proto::Status>(&m)) {
                std::printf("status: thermal=%u battery=%u%% bitrate=%u kbps fps=%.2f\n", st->thermal_state,
                            st->battery_percent, st->actual_bitrate_kbps, st->actual_fps_x100 / 100.0);
            } else if (auto e = std::get_if<proto::Error>(&m)) {
                std::printf("device error %u: %s\n", unsigned(e->code), e->message.c_str());
            }
        }
        const auto now = Clock::now();
        if (now - lastReport >= std::chrono::seconds(1)) {
            const double dt = std::chrono::duration<double>(now - lastReport).count();
            if (a.decode)
                std::printf("%.1f fps, %.2f Mbps, decoded %.1f fps (%s)\n", windowFrames / dt,
                            windowBytes * 8 / dt / 1e6, window_decoded / dt,
                            decoder.IsHardware() ? "hardware" : "software");
            else
                std::printf("%.1f fps, %.2f Mbps\n", windowFrames / dt, windowBytes * 8 / dt / 1e6);
            windowFrames = windowBytes = window_decoded = 0;
            lastReport = now;
            Send(*s, proto::Heartbeat{uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                                   now.time_since_epoch())
                                                   .count())});
        }
    }
    Send(*s, proto::Stop{});
    if (dump) fclose(dump);
    std::printf("total: %llu frames (%llu keyframes), %.1f MB\n", frames, keyframes, bytes / 1e6);
    if (a.decode) std::printf("decoded: %llu frames, %llu decode errors\n", decoded, decode_errors);
    if (!a.snapshot.empty() && last.width)
        std::printf("snapshot %s: %s\n", a.snapshot.c_str(),
                    image::SaveBmp(last.View(), a.snapshot.c_str()) ? "ok" : "failed");
    return frames > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    net::Init();
    const Args a = Parse(argc, argv);
    if (a.command == "list") return List();
    if (a.command == "echo") return Echo(a);
    if (a.command == "stream") return Stream(a);
    std::fprintf(stderr, "tcprobe %.*s: unknown command '%s' (list | stream | echo)\n", int(Version().size()),
                 Version().data(), a.command.c_str());
    return 1;
}
