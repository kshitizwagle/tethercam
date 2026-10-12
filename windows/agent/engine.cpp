#include "engine.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <format>

#include "tethercam/image.h"
#include "tethercam/log.h"
#include "tethercam/media.h"
#include "tethercam/usbmux.h"
#include "tethercam/version.h"

namespace tethercam::agent {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kSilenceTimeout = std::chrono::seconds(3);
constexpr auto kConsumerGrace = std::chrono::seconds(2);

const char* StatusName(shm::AgentStatus s) {
    switch (s) {
    case shm::AgentStatus::None: return "none";
    case shm::AgentStatus::NoDevice: return "waiting for iPhone";
    case shm::AgentStatus::AppNotOpen: return "iPhone found, app not open";
    case shm::AgentStatus::Connected: return "connected";
    case shm::AgentStatus::Streaming: return "streaming";
    case shm::AgentStatus::PausedOnPhone: return "paused on iPhone";
    case shm::AgentStatus::NoUsbmuxd: return "Apple Devices not installed";
    }
    return "?";
}

bool SleepUnless(const std::atomic<bool>& stop, int ms) {
    for (int t = 0; t < ms && !stop; t += 50) Sleep(50);
    return !stop;
}

}  // namespace

void Engine::Start(std::function<void()> on_change) {
    on_change_ = std::move(on_change);
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void Engine::StopThread() {
    stop_ = true;
    if (auto* s = live_socket_.load()) s->Shutdown();
    if (thread_.joinable()) thread_.join();
}

Snapshot Engine::Get() const {
    std::lock_guard lock(mu_);
    return snap_;
}

void Engine::SelectCamera(int camera_id) {
    {
        std::lock_guard lock(mu_);
        snap_.selected_camera = camera_id;
    }
    camera_dirty_ = true;
}

void Engine::SetMirror(bool mirror) {
    {
        std::lock_guard lock(mu_);
        snap_.mirror = mirror;
    }
    options_dirty_ = true;
}

void Engine::SetRotation(uint16_t degrees) {
    {
        std::lock_guard lock(mu_);
        snap_.rotation = degrees;
    }
    options_dirty_ = true;
}

void Engine::Notify() {
    if (on_change_) on_change_();
}

void Engine::SetStatus(shm::AgentStatus s, const std::string& detail) {
    bool changed;
    {
        std::lock_guard lock(mu_);
        changed = snap_.status != s || snap_.detail != detail;
        if (snap_.status != s) log::Info("status: {}{}{}", StatusName(s), detail.empty() ? "" : " - ", detail);
        snap_.status = s;
        snap_.detail = detail;
    }
    if (shm_open_) {
        auto* h = static_cast<shm::Header*>(shm_.view);
        shm::Atomic(h->agent_status).store(static_cast<uint32_t>(s), std::memory_order_release);
    }
    if (changed) Notify();
}

Engine::Consumer Engine::TickShm() {
    Consumer c;
    const uint64_t now = GetTickCount64();
    if (!shm_open_ && now - last_shm_try_ >= 500) {
        last_shm_try_ = now;
        shm_open_ = shm::Open(shm_);
        const DWORD err = shm_open_ ? 0 : GetLastError();
        if (shm_open_) log::Info("virtual camera shared memory opened");
        else if (err != last_shm_error_ && err != ERROR_FILE_NOT_FOUND)
            log::Warn("cannot open the virtual camera's shared memory (error {})", err);
        last_shm_error_ = err;
    }
    if (!shm_open_) return c;
    auto* h = static_cast<shm::Header*>(shm_.view);
    shm::Atomic(h->agent_pid).store(GetCurrentProcessId(), std::memory_order_relaxed);
    shm::Atomic(h->agent_status).store(static_cast<uint32_t>(Get().status), std::memory_order_relaxed);
    shm::Atomic(h->agent_heartbeat_ms).store(now, std::memory_order_release);
    c.active = shm::Atomic(h->consumer_active).load(std::memory_order_acquire) != 0;
    c.format = {h->req_width, h->req_height, h->req_fps};
    char msg[sizeof(h->vcam_message)];
    std::memcpy(msg, h->vcam_message, sizeof(msg));
    msg[sizeof(msg) - 1] = 0;
    if (last_vcam_message_ != msg) {
        last_vcam_message_ = msg;
        if (!last_vcam_message_.empty()) log::Info("[vcam] {}", last_vcam_message_);
    }
    return c;
}

bool Engine::OpenLink(net::Socket& out) {
    if (!opt_.tcp.empty()) {
        const auto colon = opt_.tcp.rfind(':');
        out = net::ConnectTcp(opt_.tcp.substr(0, colon), static_cast<uint16_t>(std::atoi(opt_.tcp.c_str() + colon + 1)));
        if (!out.Valid()) {
            SetStatus(shm::AgentStatus::NoDevice, "no device at " + opt_.tcp);
            return false;
        }
        out.SetNoDelay();
        return true;
    }
    auto devices = usbmux::Client::ListDevices();
    if (!devices) {
        SetStatus(shm::AgentStatus::NoUsbmuxd);
        return false;
    }
    bool any_usb = false;
    for (const auto& d : *devices) {
        if (d.connection_type != "USB") continue;  // wired only (PRODUCT.md)
        any_usb = true;
        const auto r = usbmux::Client::Connect(d.device_id, proto::kDevicePort, out);
        if (r == usbmux::ConnectResult::Ok) {
            log::Info("connected to device {} ({}) over USB", d.device_id, d.serial);
            return true;
        }
        SetStatus(shm::AgentStatus::AppNotOpen, usbmux::ToString(r));
    }
    if (!any_usb) SetStatus(shm::AgentStatus::NoDevice);
    return false;
}

void Engine::Run() {
    net::Init();
    media::MfScope mf;
    log::Info("Tethercam agent {} started{}", Version(), opt_.tcp.empty() ? "" : " (TCP mode: " + opt_.tcp + ")");
    while (!stop_) {
        TickShm();
        net::Socket sock;
        if (OpenLink(sock)) {
            live_socket_ = &sock;
            RunSession(sock);
            live_socket_ = nullptr;
            {
                std::lock_guard lock(mu_);
                snap_.cameras.clear();
                snap_.device_name.clear();
            }
            Notify();
            continue;
        }
        // Keep the heartbeat alive while waiting, so the camera shows the right placeholder.
        for (int i = 0; i < 4 && !stop_; ++i) {
            TickShm();
            SleepUnless(stop_, 250);
        }
    }
    if (shm_open_) {
        auto* h = static_cast<shm::Header*>(shm_.view);
        shm::Atomic(h->agent_status).store(static_cast<uint32_t>(shm::AgentStatus::None));
        shm::Atomic(h->agent_heartbeat_ms).store(0);
    }
    log::Info("agent stopped");
}

void Engine::RunSession(net::Socket& sock) {
    auto send = [&](const proto::Message& m) { return sock.SendAll(proto::Encode(m)); };
    proto::Hello hello;
    hello.role = proto::Role::Host;
    hello.app_version = std::string(Version());
    hello.name = "Tethercam for Windows";
    hello.model = "Windows";
    if (!send(hello)) return;
    SetStatus(shm::AgentStatus::Connected);

    proto::StreamParser parser;
    std::vector<uint8_t> buf(1 << 20);
    media::H264Decoder decoder;
    bool decoder_ready = false;
    std::vector<uint8_t> parameter_sets;
    bool waiting_for_key = true;
    image::Nv12Frame scaled;

    bool have_caps = false;
    bool streaming = false;       // we sent Start
    bool paused_on_phone = false;
    MediaFormat target;           // size the virtual camera asked for
    MediaFormat device_format;    // what we asked the phone for
    uint8_t camera_id = 0;
    auto last_rx = Clock::now();
    auto last_beat = Clock::now();
    auto consumer_gone_since = Clock::time_point{};
    auto window_start = Clock::now();
    uint64_t window_bytes = 0, window_frames = 0;
    uint64_t frame_counter = 0;

    auto choose_camera = [&]() -> const proto::Camera* {
        std::lock_guard lock(mu_);
        const proto::Camera* pick = nullptr;
        for (const auto& c : snap_.cameras)
            if (snap_.selected_camera == c.camera_id) pick = &c;
        if (!pick && !snap_.cameras.empty()) {
            // Default: the back wide camera, else the first one.
            for (const auto& c : snap_.cameras)
                if (c.lens == proto::Lens::Wide && c.position == proto::Position::Back) pick = &c;
            if (!pick) pick = &snap_.cameras.front();
            snap_.selected_camera = pick->camera_id;
        }
        return pick;
    };

    auto start_stream = [&](const MediaFormat& want) -> bool {
        const proto::Camera* cam = choose_camera();
        if (!cam) return false;
        MediaFormat chosen;
        if (!ChooseDeviceFormat(cam->formats, want, chosen)) return false;
        camera_id = cam->camera_id;
        proto::SetFormat sf;
        sf.width = static_cast<uint16_t>(chosen.width);
        sf.height = static_cast<uint16_t>(chosen.height);
        sf.fps = static_cast<uint16_t>(chosen.fps);
        sf.bitrate_kbps = DefaultBitrateKbps(chosen.width, chosen.height, chosen.fps);
        log::Info("requesting {} {}x{} @ {} ({} kbps) for a {}x{} @ {} consumer", cam->label, chosen.width,
                  chosen.height, chosen.fps, sf.bitrate_kbps, want.width, want.height, want.fps);
        Snapshot s = Get();
        proto::SetOptions opts{uint8_t(s.mirror), s.rotation, 0};
        device_format = chosen;
        target = want;
        waiting_for_key = true;
        return send(proto::SetCamera{camera_id}) && send(opts) && send(sf) && send(proto::Start{});
    };

    while (!stop_) {
        // 1. Read whatever the phone sent.
        const int w = sock.WaitReadable(20);
        if (w < 0) break;
        if (w > 0) {
            const int n = sock.RecvSome(buf);
            if (n <= 0) {
                log::Info("device disconnected");
                break;
            }
            last_rx = Clock::now();
            parser.Feed({buf.data(), size_t(n)});
        }
        proto::Message m;
        bool fatal = false;
        for (;;) {
            const auto r = parser.Next(m);
            if (r == proto::StreamParser::Result::NeedMore) break;
            if (r == proto::StreamParser::Result::Fatal) {
                log::Error("corrupt stream from device; reconnecting");
                fatal = true;
                break;
            }
            if (auto h = std::get_if<proto::Hello>(&m)) {
                log::Info("device: {} ({}), app {}, protocol {}-{}", h->name, h->model, h->app_version,
                          h->min_version, h->max_version);
                if (h->max_version < proto::kVersion || h->min_version > proto::kVersion) {
                    send(proto::Error{proto::ErrorCode::UnsupportedVersion, "protocol version mismatch"});
                    fatal = true;
                    break;
                }
                std::lock_guard lock(mu_);
                snap_.device_name = h->name;
            } else if (auto caps = std::get_if<proto::Caps>(&m)) {
                {
                    std::lock_guard lock(mu_);
                    snap_.cameras = caps->cameras;
                }
                for (const auto& c : caps->cameras) {
                    std::string list;
                    for (const auto& f : c.formats) list += std::format(" {}x{}@{}", f.width, f.height, f.max_fps);
                    log::Info("camera {} '{}':{}", c.camera_id, c.label, list);
                }
                have_caps = true;
                streaming = false;  // (re)start with the new capabilities if a consumer is waiting
                Notify();
            } else if (auto vc = std::get_if<proto::VideoConfig>(&m)) {
                parameter_sets = vc->parameter_sets;
                decoder_ready = decoder.Init(vc->width, vc->height, vc->fps, opt_.prefer_hardware_decode);
                waiting_for_key = true;
                paused_on_phone = false;
                log::Info("video: {}x{} @ {}", vc->width, vc->height, vc->fps);
            } else if (auto vf = std::get_if<proto::VideoFrame>(&m)) {
                ++window_frames;
                window_bytes += vf->data.size();
                if (!decoder_ready || !streaming) continue;
                if (waiting_for_key && !vf->IsKeyframe()) continue;
                waiting_for_key = false;
                std::vector<uint8_t> au;
                if (vf->IsKeyframe()) au = parameter_sets;
                au.insert(au.end(), vf->data.begin(), vf->data.end());
                const bool ok = decoder.Decode(au, int64_t(vf->pts_us * 10), [&](const image::Nv12View& v, int64_t) {
                    if (!shm_open_ || target.width == 0) return;
                    ++frame_counter;
                    if (v.width == target.width && v.height == target.height) {
                        shm::WriteFrame(shm_.view, v.width, v.height, v.y, v.y_pitch, v.uv, v.uv_pitch,
                                        GetTickCount64() * 10000);
                    } else {
                        scaled.Resize(target.width, target.height);
                        image::ScaleLetterbox(v, scaled);
                        const auto sv = scaled.View();
                        shm::WriteFrame(shm_.view, sv.width, sv.height, sv.y, sv.y_pitch, sv.uv, sv.uv_pitch,
                                        GetTickCount64() * 10000);
                    }
                    SetEvent(shm_.frame_ready);
                });
                if (!ok) {
                    log::Warn("decode error; requesting keyframe");
                    waiting_for_key = true;
                    decoder.Flush();
                    send(proto::RequestKeyframe{});
                }
            } else if (auto st = std::get_if<proto::Status>(&m)) {
                if (st->thermal_state >= 2) log::Warn("iPhone thermal state {} (serious or worse)", st->thermal_state);
            } else if (auto e = std::get_if<proto::Error>(&m)) {
                log::Warn("device error {}: {}", unsigned(e->code), e->message);
                if (e->code == proto::ErrorCode::CameraUnavailable || e->code == proto::ErrorCode::ThermalShutdown) {
                    paused_on_phone = true;
                }
            }
        }
        if (fatal) break;
        const auto now = Clock::now();
        if (now - last_rx > kSilenceTimeout) {
            log::Warn("no data from device for 3 s; reconnecting");
            break;
        }

        // 2. React to what the virtual camera's consumers want.
        const Consumer consumer = TickShm();
        if (have_caps) {
            if (consumer.active && consumer.format.width) {
                consumer_gone_since = {};
                if (!streaming || camera_dirty_.exchange(false) || !(consumer.format == target)) {
                    streaming = start_stream(consumer.format);
                    if (!streaming) {
                        log::Error("could not start the stream");
                        break;
                    }
                }
            } else if (streaming) {
                if (consumer_gone_since == Clock::time_point{}) consumer_gone_since = now;
                if (now - consumer_gone_since > kConsumerGrace) {
                    log::Info("no app is using the camera; stopping the iPhone camera");
                    send(proto::Stop{});
                    streaming = false;
                    target = {};
                }
            }
        }
        if (options_dirty_.exchange(false)) {
            Snapshot s = Get();
            send(proto::SetOptions{uint8_t(s.mirror), s.rotation, 0});
        }

        // 3. Status and heartbeats.
        if (now - last_beat >= std::chrono::seconds(1)) {
            last_beat = now;
            send(proto::Heartbeat{uint64_t(
                std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count())});
        }
        if (now - window_start >= std::chrono::seconds(1)) {
            const double dt = std::chrono::duration<double>(now - window_start).count();
            std::string detail;
            shm::AgentStatus st = shm::AgentStatus::Connected;
            if (paused_on_phone) st = shm::AgentStatus::PausedOnPhone;
            else if (streaming) {
                st = shm::AgentStatus::Streaming;
                detail = std::format("{}x{} @ {} fps, {:.1f} Mbps{}", device_format.width, device_format.height,
                                     static_cast<int>(window_frames / dt + 0.5), window_bytes * 8 / dt / 1e6,
                                     decoder_ready && !decoder.IsHardware() ? " (software decode)" : "");
            }
            SetStatus(st, detail);
            window_start = now;
            window_bytes = window_frames = 0;
        }
    }
    if (streaming) send(proto::Stop{});
}

}  // namespace tethercam::agent
