#include "engine.h"

#include <sys/resource.h>

#include <algorithm>
#include <cstdlib>
#include <format>

#include "decoder.h"
#include "tethercam/image.h"
#include "tethercam/log.h"
#include "tethercam/usbmux.h"
#include "tethercam/version.h"

namespace tethercam::agent {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kSilenceTimeout = std::chrono::seconds(3);
constexpr auto kConsumerGrace = std::chrono::seconds(2);
constexpr auto kOpenerScan = std::chrono::milliseconds(500);
// While there's no live video, readers still get a frame this often (and never stall).
constexpr auto kIdleFrame = std::chrono::milliseconds(100);
// A stall shorter than this holds the last live frame; longer goes black (nothing looks frozen).
constexpr auto kStaleFrame = std::chrono::milliseconds(750);
constexpr MediaFormat kAutoFormat{1920, 1080, 30};
constexpr auto kWifiRetry = std::chrono::seconds(3);
constexpr auto kUsbCheck = std::chrono::seconds(2);

bool SleepUnless(const std::atomic<bool>& stop, int ms) {
    for (int t = 0; t < ms && !stop; t += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return !stop;
}

}  // namespace

void Engine::Start(std::function<void()> on_change) {
    on_change_ = std::move(on_change);
    stop_ = false;
    {
        std::lock_guard lock(mu_);
        preferred_camera_ = settings_.Get("Camera");
        snap_.mirror = settings_.Get("Mirror") == "1";
        snap_.rotation = static_cast<uint16_t>(std::atoi(settings_.Get("Rotation").c_str()) % 360);
        const int quality = std::atoi(settings_.Get("Quality").c_str());
        snap_.quality = quality >= 0 && quality <= 4 ? proto::Quality(quality) : proto::Quality::Auto;
        quality_dirty_ = settings_.Get("QualityPending") == "1";
        mirror_dirty_ = settings_.Get("MirrorPending") == "1";
        options_dirty_ = mirror_dirty_.load();
        pairing_ = wifi::PairingStore(opt_.pairing_file.empty() ? wifi::PairingStore::DefaultPath() : opt_.pairing_file);
        if (!pairing_.Load()) log::Warn("cannot read the Wi-Fi pairing file; starting without pairings");
        UpdateWifiSnapshotLocked();
    }
    thread_ = std::thread([this] { Run(); });
}

void Engine::StopThread() {
    stop_ = true;
    if (auto* l = live_link_.load()) l->Shutdown();
    if (thread_.joinable()) thread_.join();
}

void Engine::UpdateWifiSnapshotLocked() {
    const auto confirmed = pairing_.Confirmed();
    snap_.wifi_phone.clear();
    snap_.wifi_address.clear();
    if (confirmed.empty()) return;
    const auto& p = *confirmed.front();
    snap_.wifi_phone = p.name.empty() ? "iPhone" : p.name;
    snap_.wifi_address = !p.manual.empty() ? p.manual : p.address;
}

void Engine::SavePairingLocked() {
    if (!pairing_.Save()) log::Warn("cannot save the Wi-Fi pairing file");
}

void Engine::ForgetWifi() {
    {
        std::lock_guard lock(mu_);
        pairing_.ForgetAll();
        SavePairingLocked();
        UpdateWifiSnapshotLocked();
    }
    log::Info("Wi-Fi pairings forgotten");
    if (wifi_session_)
        if (auto* l = live_link_.load()) l->Shutdown();
    Notify();
}

void Engine::SetWifiAddress(const std::string& address) {
    {
        std::lock_guard lock(mu_);
        const auto confirmed = pairing_.Confirmed();
        if (confirmed.empty()) return;
        pairing_.SetManual(confirmed.front()->serial, address);
        SavePairingLocked();
        UpdateWifiSnapshotLocked();
    }
    Notify();
}

Snapshot Engine::Get() const {
    std::lock_guard lock(mu_);
    return snap_;
}

std::string Engine::CameraDevice() const {
    std::lock_guard lock(mu_);
    return camera_device_;
}

void Engine::SelectCamera(int camera_id) {
    {
        std::lock_guard lock(mu_);
        snap_.selected_camera = camera_id;
        RememberCameraLocked();
    }
    camera_dirty_ = true;
    Notify();
}

void Engine::RememberCameraLocked() {
    for (const auto& c : snap_.cameras) {
        if (c.camera_id == snap_.selected_camera) {
            preferred_camera_ = c.label;
            settings_.Set("Camera", c.label);
        }
    }
}

void Engine::SetMirror(bool mirror) {
    {
        std::lock_guard lock(mu_);
        snap_.mirror = mirror;
    }
    settings_.Set("Mirror", mirror ? "1" : "0");
    settings_.Set("MirrorPending", "1");
    mirror_dirty_ = true;
    options_dirty_ = true;
    Notify();
}

bool Engine::CopyPreview(image::Nv12Frame& out, uint64_t& seq) const {
    std::lock_guard lock(preview_mu_);
    if (preview_seq_ == seq || preview_.width == 0) return false;
    out = preview_;
    seq = preview_seq_;
    return true;
}

void Engine::SetRotation(uint16_t degrees) {
    {
        std::lock_guard lock(mu_);
        snap_.rotation = degrees;
    }
    settings_.Set("Rotation", std::to_string(degrees));
    options_dirty_ = true;
    Notify();
}

void Engine::SetQuality(proto::Quality quality) {
    if (uint8_t(quality) > uint8_t(proto::Quality::UHD60)) return;
    {
        std::lock_guard lock(mu_);
        snap_.quality = quality;
        settings_.Set("Quality", std::to_string(uint8_t(quality)));
        settings_.Set("QualityPending", "1");
        quality_dirty_ = true;
    }
    Notify();
}

std::string Engine::StatusJson() const {
    std::lock_guard lock(mu_);
    return agent::StatusJson(snap_);
}

void Engine::Notify() {
    if (on_change_) on_change_();
}

void Engine::SetStatus(AgentStatus s, const std::string& detail) {
    bool changed;
    {
        std::lock_guard lock(mu_);
        changed = snap_.status != s || snap_.detail != detail;
        if (snap_.status != s) log::Info("status: {}{}{}", StatusName(s), detail.empty() ? "" : " - ", detail);
        snap_.status = s;
        snap_.detail = detail;
    }
    if (changed) Notify();
}

MediaFormat Engine::OutputFormat() const {
    // One format for every app (R9): the quality setting, else 1080p30.
    return QualityFormat(Get().quality, kAutoFormat);
}

Engine::Consumer Engine::TickCamera() {
    const auto now = Clock::now();
    const MediaFormat want = OutputFormat();
    // (Re)open the device, or switch its format while no app is reading it.
    if ((!out_.IsOpen() || (!(out_.Format() == want) && openers_ == 0)) && now - last_open_try_ >= std::chrono::seconds(1)) {
        last_open_try_ = now;
        const std::string dev = opt_.device.empty() ? FindLoopbackDevice() : opt_.device;
        if (dev.empty()) {
            if (!warned_no_device_)
                log::Warn("no v4l2loopback device: run 'sudo modprobe v4l2loopback' (see docs/USER_GUIDE.md)");
            warned_no_device_ = true;
        } else {
            warned_no_device_ = false;
            out_.Close();  // a format change needs a fresh producer
            if (out_.Open(dev, want, opt_.pixel_format)) out_.WriteBlack();
        }
        {
            std::lock_guard lock(mu_);
            camera_device_ = out_.IsOpen() ? out_.Device() : std::string{};
        }
    }
    if (out_.IsOpen() && now - last_scan_ >= kOpenerScan) {
        last_scan_ = now;
        const int n = CountOtherOpeners(out_.Device());
        if (n != openers_) log::Info("{} app(s) using the camera", n);
        openers_ = n;
    }
    Consumer c;
    c.active = out_.IsOpen() && (openers_ > 0 || opt_.always_on);
    c.format = out_.Format();
    return c;
}

void Engine::FeedIdle() {
    const auto now = Clock::now();
    // Fires only when nothing at all went to the camera recently (live frames count too).
    if (!out_.IsOpen() || now - last_any_write_ < kIdleFrame) return;
    last_any_write_ = now;
    // A short stall (a phone hiccup, a keyframe wait): repeat the last frame instead of flickering
    // black over live video. Real idle still goes black, so nothing looks frozen.
    if (last_write_ != Clock::time_point{} && now - last_write_ < kStaleFrame) out_.Repeat();
    else out_.WriteBlack();
}

bool Engine::OpenLink(std::unique_ptr<Link>& out) {
    link_serial_.clear();
    if (!opt_.tcp.empty()) {
        const auto colon = opt_.tcp.rfind(':');
        net::Socket sock =
            net::ConnectTcp(opt_.tcp.substr(0, colon), static_cast<uint16_t>(std::atoi(opt_.tcp.c_str() + colon + 1)));
        if (!sock.Valid()) {
            SetStatus(AgentStatus::NoDevice, "no device at " + opt_.tcp);
            return false;
        }
        sock.SetNoDelay();
        out = std::make_unique<PlainLink>(std::move(sock));
        link_serial_ = "tcp:" + opt_.tcp;  // stands in for USB, so fakephone can pair
        return true;
    }
    if (!opt_.wifi.empty()) return OpenWifi(out);
    const auto devices = usbmux::Client::ListDevices();
    bool any_usb = false;
    if (devices) {
        for (const auto& d : *devices) {
            // usbmuxd's network entries can't reach our port; Wi-Fi uses our own encrypted transport.
            if (d.connection_type != "USB") continue;
            any_usb = true;
            net::Socket sock;
            const auto r = usbmux::Client::Connect(d.device_id, proto::kDevicePort, sock);
            if (r == usbmux::ConnectResult::Ok) {
                log::Info("connected to device {} ({}) over USB", d.device_id, d.serial);
                out = std::make_unique<PlainLink>(std::move(sock));
                link_serial_ = d.serial;
                return true;
            }
            SetStatus(AgentStatus::AppNotOpen, usbmux::ToString(r));
        }
    }
    // USB wins (F12); with no phone on USB, a phone paired for Wi-Fi may be on the network.
    if (!any_usb && OpenWifi(out)) return true;
    // On Linux, udev starts usbmuxd when an iPhone is plugged in, so "not running" usually just
    // means no phone yet.
    if (!devices) SetStatus(AgentStatus::NoDevice, "usbmuxd not running; plug in the iPhone");
    else if (!any_usb) SetStatus(AgentStatus::NoDevice);
    return false;
}

bool Engine::OpenWifi(std::unique_ptr<Link>& out) {
    const auto now = Clock::now();
    if (opt_.wifi.empty() && now - last_wifi_try_ < kWifiRetry) return false;
    std::vector<wifi::Candidate> candidates;
    wifi::HostId host;
    {
        std::lock_guard lock(mu_);
        if (pairing_.Confirmed().empty()) return false;
        host = pairing_.host_id();
        if (!discovery_ && opt_.wifi.empty()) discovery_ = wifi::StartDiscovery();
        const auto found = discovery_ ? discovery_->Found() : std::vector<wifi::Discovered>{};
        candidates = wifi::Candidates(pairing_, found, opt_.wifi);
    }
    last_wifi_try_ = now;
    for (const auto& c : candidates) {
        if (stop_) return false;
        TickCamera();  // an absent phone costs up to a connect timeout per address
        FeedIdle();
        net::Socket sock = net::ConnectTcp(c.host, c.port, 700);
        if (!sock.Valid()) continue;
        sock.SetNoDelay();
        auto link = wifi::ConnectHost(std::move(sock), host, c.phone.key, 2000);
        const std::string where = std::format("{} at {}:{}", c.phone.name, c.host, c.port);
        if (!link) {
            if (last_wifi_warning_ != where)
                log::Warn("{} refused the Wi-Fi connection (pairing forgotten on the iPhone, or Wireless off); "
                          "connect it by USB once to pair again", where);
            last_wifi_warning_ = where;
            continue;
        }
        last_wifi_warning_.clear();
        log::Info("connected to {} over Wi-Fi", where);
        out = std::move(link);
        return true;
    }
    if (!opt_.wifi.empty()) SetStatus(AgentStatus::NoDevice, "no paired phone at " + opt_.wifi);
    return false;
}

void Engine::Run() {
    net::Init();
    // Keep the decode/delivery loop schedulable on a loaded machine (the systemd unit raises
    // CPUWeight as well). Silently ignored when RLIMIT_NICE doesn't allow it.
    if (::setpriority(PRIO_PROCESS, 0, -5) == 0) log::Info("media priority: nice -5");
    log::Info("Tethercam agent {} started{}{}", Version(), opt_.tcp.empty() ? "" : " (TCP mode: " + opt_.tcp + ")",
              opt_.wifi.empty() ? "" : " (Wi-Fi: " + opt_.wifi + ")");
    while (!stop_) {
        TickCamera();
        FeedIdle();
        std::unique_ptr<Link> link;
        if (OpenLink(link)) {
            live_link_ = link.get();
            wifi_session_ = link->Encrypted();
            RunSession(*link);
            live_link_ = nullptr;
            wifi_session_ = false;
            link.reset();
            {
                std::lock_guard lock(mu_);
                snap_.cameras.clear();
                snap_.device_name.clear();
                snap_.transport.clear();
                snap_.width = snap_.height = snap_.fps = 0;
            }
            Notify();
            continue;
        }
        // Keep feeding the camera while waiting, so apps show black instead of freezing.
        for (int i = 0; i < 10 && !stop_; ++i) {
            TickCamera();
            FeedIdle();
            SleepUnless(stop_, 100);
        }
    }
    out_.Close();
    log::Info("agent stopped");
}

void Engine::RunSession(Link& link) {
    auto send = [&](const proto::Message& m) { return link.SendAll(proto::Encode(m)); };
    const bool wifi = link.Encrypted();
    proto::Hello hello;
    hello.role = proto::Role::Host;
    hello.app_version = std::string(Version());
    hello.name = "Tethercam for Linux";
    hello.model = "Linux";
    if (!send(hello)) return;
    {
        std::lock_guard lock(mu_);
        snap_.transport = wifi ? "wifi" : "usb";
    }
    // Pairing happens only over USB (the trusted link): offer this phone's key every time. The phone
    // stores it if Wireless is on and answers WifiPaired; older apps ignore the message.
    if (!wifi && !link_serial_.empty()) {
        proto::WifiPair pair;
        {
            std::lock_guard lock(mu_);
            pair.host_id = pairing_.host_id();
            pair.key = pairing_.ForSerial(link_serial_).key;
        }
        pair.name = wifi::LocalHostName();
        if (!send(pair)) return;
    }
    SetStatus(AgentStatus::Connected);

    proto::StreamParser parser;
    std::vector<uint8_t> buf(1 << 20);
    media::H264Decoder decoder;
    bool decoder_ready = false;
    std::vector<uint8_t> parameter_sets;
    bool waiting_for_key = true;

    bool have_caps = false;
    bool streaming = false;
    bool paused_on_phone = false;
    MediaFormat target;
    MediaFormat device_format;
    uint8_t camera_id = 0;
    auto last_rx = Clock::now();
    auto last_beat = Clock::now();
    auto last_usb_check = Clock::now();
    auto consumer_gone_since = Clock::time_point{};
    auto window_start = Clock::now();
    uint64_t window_bytes = 0, window_frames = 0;
    int64_t lag_sum_us = 0, lag_max_us = INT64_MIN;

    auto choose_camera = [&]() -> const proto::Camera* {
        std::lock_guard lock(mu_);
        const proto::Camera* pick = nullptr;
        for (const auto& c : snap_.cameras)
            if (snap_.selected_camera == c.camera_id) pick = &c;
        if (!pick && !snap_.cameras.empty()) {
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
        const Snapshot s = Get();
        MediaFormat request = QualityFormat(s.quality, want);
        // Over Wi-Fi, Auto stays at 1080p (R11); an explicit preset is honoured.
        if (wifi && s.quality == proto::Quality::Auto && request.height > 1080)
            request = {1920, 1080, std::min<uint32_t>(request.fps, 60)};
        if (!ChooseDeviceFormat(cam->formats, request, chosen)) return false;
        camera_id = cam->camera_id;
        proto::SetFormat sf;
        sf.width = static_cast<uint16_t>(chosen.width);
        sf.height = static_cast<uint16_t>(chosen.height);
        sf.fps = static_cast<uint16_t>(chosen.fps);
        sf.bitrate_kbps = DefaultBitrateKbps(chosen.width, chosen.height, chosen.fps);
        log::Info("requesting {} {}x{} @ {} ({} kbps) for a {}x{} @ {} camera", cam->label, chosen.width,
                  chosen.height, chosen.fps, sf.bitrate_kbps, want.width, want.height, want.fps);
        proto::SetOptions opts{uint8_t(s.mirror), s.rotation, 0};
        device_format = chosen;
        target = want;
        waiting_for_key = true;
        return send(proto::Stop{}) && send(proto::SetCamera{camera_id}) && send(opts) && send(sf) && send(proto::Start{});
    };

    while (!stop_) {
        // 1. Read whatever the phone sent.
        const int w = link.WaitReadable(20);
        if (w < 0) break;
        if (w > 0) {
            const int n = link.RecvSome(buf);
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
            } else if (auto quality = std::get_if<proto::SetQuality>(&m)) {
                {
                    std::lock_guard lock(mu_);
                    if (!quality_dirty_) {
                        if (snap_.quality != quality->quality) camera_dirty_ = true;
                        snap_.quality = quality->quality;
                        settings_.Set("Quality", std::to_string(uint8_t(snap_.quality)));
                    }
                }
                Notify();
            } else if (auto opts = std::get_if<proto::SetOptions>(&m)) {
                // The phone's mirror (saved there, or toggled on the phone); rotation and dimming stay ours.
                // An offline PC choice wins the next connection, like quality.
                bool changed = false;
                {
                    std::lock_guard lock(mu_);
                    if (!mirror_dirty_ && snap_.mirror != (opts->mirror != 0)) {
                        snap_.mirror = opts->mirror != 0;
                        settings_.Set("Mirror", snap_.mirror ? "1" : "0");
                        changed = true;
                    }
                }
                if (changed) Notify();
            } else if (auto caps = std::get_if<proto::Caps>(&m)) {
                {
                    std::lock_guard lock(mu_);
                    snap_.cameras = caps->cameras;
                    snap_.selected_camera = -1;
                    for (const auto& c : caps->cameras)
                        if (!preferred_camera_.empty() && c.label == preferred_camera_) snap_.selected_camera = c.camera_id;
                }
                for (const auto& c : caps->cameras) {
                    std::string list;
                    for (const auto& f : c.formats) list += std::format(" {}x{}@{}", f.width, f.height, f.max_fps);
                    log::Info("camera {} '{}':{}", c.camera_id, c.label, list);
                }
                have_caps = true;
                streaming = false;
                Notify();
            } else if (auto vc = std::get_if<proto::VideoConfig>(&m)) {
                parameter_sets = vc->parameter_sets;
                decoder_ready = decoder.Init(opt_.prefer_hardware_decode);
                waiting_for_key = true;
                paused_on_phone = false;
                device_format = {vc->width, vc->height, vc->fps};
                log::Info("video: {}x{} @ {}", vc->width, vc->height, vc->fps);
            } else if (auto vf = std::get_if<proto::VideoFrame>(&m)) {
                ++window_frames;
                window_bytes += vf->data.size();
                const int64_t lag = std::chrono::duration_cast<std::chrono::microseconds>(
                                        Clock::now().time_since_epoch()).count() - int64_t(vf->pts_us);
                if (lag < lag_floor_us_) lag_floor_us_ = lag;
                lag_sum_us += lag;
                lag_max_us = std::max(lag_max_us, lag);
                if (!decoder_ready || !streaming) continue;
                if (waiting_for_key && !vf->IsKeyframe()) continue;
                waiting_for_key = false;
                std::vector<uint8_t> au;
                if (vf->IsKeyframe()) au = parameter_sets;
                au.insert(au.end(), vf->data.begin(), vf->data.end());
                const bool ok = decoder.Decode(au, int64_t(vf->pts_us), [&](const image::Nv12View& v, int64_t) {
                    if (preview_active_) {
                        // Keep a small copy for the preview window, at the source's aspect ratio.
                        const uint32_t pw = std::min<uint32_t>(1280, v.width) & ~1u;
                        const uint32_t ph = static_cast<uint32_t>(uint64_t(pw) * v.height / v.width) & ~1u;
                        std::lock_guard lock(preview_mu_);
                        if (preview_.width != pw || preview_.height != ph) preview_.Resize(pw, ph);
                        image::ScaleLetterbox(v, preview_);
                        ++preview_seq_;
                    }
                    if (out_.IsOpen() && out_.Write(v)) {
                        last_write_ = Clock::now();
                        last_any_write_ = last_write_;
                    }
                });
                if (!ok) {
                    log::Warn("decode error; requesting keyframe");
                    waiting_for_key = true;
                    decoder.Flush();
                    send(proto::RequestKeyframe{});
                }
            } else if (auto st = std::get_if<proto::Status>(&m)) {
                {
                    std::lock_guard lock(mu_);
                    if (snap_.thermal != st->thermal_state)
                        log::Info("iPhone thermal state {} -> {} (0 nominal, 1 fair, 2 serious, 3 critical), battery {}%",
                                  snap_.thermal, st->thermal_state, st->battery_percent);
                    snap_.thermal = st->thermal_state;
                    snap_.battery = st->battery_percent == 255 ? -1 : st->battery_percent;
                    snap_.charging = st->charging != 0;
                }
                if (st->camera_id != 255) {
                    bool changed = false;
                    {
                        std::lock_guard lock(mu_);
                        if (snap_.selected_camera != st->camera_id) {
                            snap_.selected_camera = st->camera_id;
                            RememberCameraLocked();
                            changed = true;
                        }
                    }
                    if (changed) {
                        camera_id = st->camera_id;
                        camera_dirty_ = true;
                        log::Info("lens changed on the iPhone to camera {}", camera_id);
                        Notify();
                    }
                }
            } else if (auto wp = std::get_if<proto::WifiPaired>(&m)) {
                if (wifi) continue;  // pairing is accepted over USB only
                bool changed = false;
                {
                    std::lock_guard lock(mu_);
                    changed = pairing_.Apply(link_serial_, *wp);
                    if (changed) {
                        SavePairingLocked();
                        UpdateWifiSnapshotLocked();
                    }
                }
                if (wp->status != proto::WifiPairStatus::Paired) log::Info("Wireless is off on the iPhone; not paired for Wi-Fi");
                else if (changed) log::Info("paired with {} for Wi-Fi ({}:{})", wp->name, wp->address, wp->port);
                if (changed) Notify();
            } else if (auto e = std::get_if<proto::Error>(&m)) {
                log::Warn("device error {}: {}", unsigned(e->code), e->message);
                if (e->code == proto::ErrorCode::CameraUnavailable || e->code == proto::ErrorCode::ThermalShutdown)
                    paused_on_phone = true;
            }
        }
        if (fatal) break;
        const auto now = Clock::now();
        if (now - last_rx > kSilenceTimeout) {
            log::Warn("no data from device for 3 s; reconnecting");
            break;
        }
        if (wifi && opt_.wifi.empty() && now - last_usb_check >= kUsbCheck) {
            last_usb_check = now;
            const auto devices = usbmux::Client::ListDevices();
            if (devices && std::any_of(devices->begin(), devices->end(),
                                       [](const usbmux::Device& d) { return d.connection_type == "USB"; })) {
                log::Info("iPhone connected by USB; switching from Wi-Fi");
                break;
            }
        }

        // 2. React to whether an app is reading the camera.
        if (have_caps && quality_dirty_) {
            const auto quality = Get().quality;
            if (!send(proto::SetQuality{quality})) break;
            {
                std::lock_guard lock(mu_);
                if (snap_.quality == quality) {
                    quality_dirty_ = false;
                    settings_.Set("QualityPending", "0");
                }
            }
            camera_dirty_ = true;
        }
        Consumer consumer = TickCamera();
        if (!(consumer.active && consumer.format.width) && preview_active_) {
            consumer.active = true;  // only the preview window is watching
            consumer.format = kAutoFormat;
        }
        if (have_caps) {
            if (consumer.active && consumer.format.width) {
                consumer_gone_since = {};
                const bool capture_changed = camera_dirty_.exchange(false);
                if (!streaming || capture_changed || !(consumer.format == target)) {
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
        // Black frames whenever no decoded frame went out recently (not streaming, paused, stalled).
        FeedIdle();
        // After Caps: by then the phone has announced its saved mirror, so we never send a stale one.
        if (have_caps && options_dirty_.exchange(false)) {
            Snapshot s = Get();
            if (send(proto::SetOptions{uint8_t(s.mirror), s.rotation, 0}) && mirror_dirty_.exchange(false))
                settings_.Set("MirrorPending", "0");
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
            AgentStatus st = AgentStatus::Connected;
            if (paused_on_phone) st = AgentStatus::PausedOnPhone;
            else if (streaming) {
                st = AgentStatus::Streaming;
                detail = std::format("{}x{} @ {} fps, {:.1f} Mbps{}", device_format.width, device_format.height,
                                     static_cast<int>(window_frames / dt + 0.5), window_bytes * 8 / dt / 1e6,
                                     decoder_ready && !decoder.IsHardware() ? " (software decode)" : "");
            }
            {
                std::lock_guard lock(mu_);
                const bool live = st == AgentStatus::Streaming && window_frames > 0;
                snap_.width = live ? device_format.width : 0;
                snap_.height = live ? device_format.height : 0;
                snap_.fps = live ? static_cast<uint32_t>(window_frames / dt + 0.5) : 0;
            }
            SetStatus(st, detail);
            if (window_frames)
                log::Debug("frame lag over {}: avg {:.1f} ms, max {:.1f} ms (relative to the lowest seen)",
                           wifi ? "Wi-Fi" : "USB", (lag_sum_us / double(window_frames) - lag_floor_us_) / 1000,
                           (lag_max_us - lag_floor_us_) / 1000.0);
            window_start = now;
            window_bytes = window_frames = 0;
            lag_sum_us = 0;
            lag_max_us = INT64_MIN;
        }
    }
    if (streaming) send(proto::Stop{});
}

}  // namespace tethercam::agent
