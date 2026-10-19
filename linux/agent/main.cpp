// tethercam-agent for Linux: connects to the iPhone over usbmuxd, decodes the video and feeds the
// v4l2loopback virtual camera. See docs/TECH_SPEC.md §13 and docs/USER_GUIDE.md.

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <thread>

#include "engine.h"
#include "settings.h"
#include "tethercam/log.h"
#include "tethercam/status_server.h"
#include "tethercam/version.h"
#ifdef TETHERCAM_TRAY
#include "tray.h"
#endif

using namespace tethercam;

namespace {

std::atomic<bool> g_quit{false};

// One agent per user: a second one would fight over the camera device and port 4790.
bool TakeInstanceLock() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/tethercam-agent.lock";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;  // fd stays open (and locked) until exit
}

void Usage() {
    std::puts(
        "Usage: tethercam-agent [options]\n"
        "  --tcp HOST:PORT     talk to fakephone over TCP instead of an iPhone over usbmuxd\n"
        "  --wifi HOST[:PORT]  connect over Wi-Fi to the paired phone at this address\n"
        "  --pairing-file P    Wi-Fi pairing file (default $XDG_CONFIG_HOME/tethercam/pairing)\n"
        "  --device /dev/videoN  v4l2loopback device (default: the one labelled \"Tethercam\")\n"
        "  --nv12              write NV12 instead of YUYV (smaller; not every app accepts it)\n"
        "  --software-decode   don't try VA-API\n"
        "  --always-on         stream even when no app seems to have the camera open\n"
        "  --preview           open the camera preview window at startup (tray mode)\n"
        "  --no-tray           run without a tray icon\n"
        "  --no-status-api     don't serve http://127.0.0.1:4790\n"
        "  --verbose           debug logging\n"
        "  --version");
}

}  // namespace

int main(int argc, char** argv) {
    agent::Engine::Options opt;
    bool tray = true, status_api = true, preview = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--tcp") opt.tcp = next();
        else if (a == "--wifi") opt.wifi = next();
        else if (a == "--pairing-file") opt.pairing_file = next();
        else if (a == "--device") opt.device = next();
        else if (a == "--nv12") opt.pixel_format = agent::PixelFormat::Nv12;
        else if (a == "--software-decode") opt.prefer_hardware_decode = false;
        else if (a == "--always-on") opt.always_on = true;
        else if (a == "--preview") preview = true;
        else if (a == "--no-tray") tray = false;
        else if (a == "--no-status-api") status_api = false;
        else if (a == "--verbose") log::SetMinLevel(log::Level::Debug);
        else if (a == "--version") {
            std::printf("tethercam-agent %s\n", std::string(Version()).c_str());
            return 0;
        } else if (a == "--help" || a == "-h") {
            Usage();
            return 0;
        } else if (a.starts_with("--gtk") || a == "--display") {
            // GTK's own options; gtk_init_check consumes them.
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            Usage();
            return 2;
        }
    }

    if (!TakeInstanceLock()) {
        std::fprintf(stderr, "tethercam-agent is already running (systemctl --user status tethercam-agent)\n");
        return 1;
    }
    const auto log_dir = agent::StateDir();
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    if (!ec) log::SetFile(log_dir / "agent.log");

    agent::Settings settings(agent::Settings::DefaultPath());
    agent::Engine engine(opt, settings);
    agent::StatusServer status([&engine] { return engine.StatusJson(); });
    if (status_api) status.Start();

#ifdef TETHERCAM_TRAY
    if (tray && agent::Tray::Available(&argc, &argv)) {
        agent::Tray t(engine, log_dir);
        t.Create();
        if (preview) t.ShowPreview();
        engine.Start([&] {
            status.Broadcast();
            t.Refresh();
        });
        t.Run();
        engine.StopThread();
        return 0;
    }
    if (tray) log::Info("no display: running without a tray icon");
#else
    if (tray) log::Info("built without tray support: running headless");
#endif

    struct sigaction sa{};
    sa.sa_handler = [](int) { g_quit = true; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    engine.Start([&] { status.Broadcast(); });
    while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    engine.StopThread();
    return 0;
}
