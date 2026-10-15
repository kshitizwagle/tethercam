// tethercam-agent: user-session tray app. Owns the USB session, decoding and the shared-memory
// writer that feeds the virtual camera (docs/DESIGN.md).
//
//   tethercam-agent                    run the tray agent
//   tethercam-agent --console          run with a console log (no tray)
//   tethercam-agent --preview          open the preview window at startup
//   tethercam-agent --tcp HOST:PORT    use plain TCP to a simulated device (fakephone)
//   tethercam-agent --session-camera   also create a temporary camera for this session (no admin)
//   tethercam-agent --install [--user] [--dll PATH]     register the virtual camera
//   tethercam-agent --uninstall [--user] [--dll PATH]   remove it
//   tethercam-agent --software-decode  disable hardware decoding

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <memory>
#include <string>

#include "engine.h"
#include "install.h"
#include "tethercam/status_server.h"
#include "tethercam/log.h"
#include "tethercam/media.h"
#include "tethercam/version.h"
#include "tray.h"

using namespace tethercam;

namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring LogDirectory() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\Tethercam\\logs";
        CoTaskMemFree(base);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    }
    return dir;
}

void AttachConsoleIfAny(bool create) {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || (create && AllocConsole())) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Crisp UI on high-DPI displays (the preview draws in DIPs with Direct2D).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool console = false, preview = false, install = false, uninstall = false, per_user = false, session_camera = false;
    agent::Engine::Options options;
    std::wstring dll = agent::DefaultDllPath();
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--console") console = true;
        else if (a == L"--preview") preview = true;
        else if (a == L"--install") install = true;
        else if (a == L"--uninstall") uninstall = true;
        else if (a == L"--user") per_user = true;
        else if (a == L"--session-camera") session_camera = true;
        else if (a == L"--software-decode") options.prefer_hardware_decode = false;
        else if (a == L"--tcp" && i + 1 < argc) options.tcp = Narrow(argv[++i]);
        else if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
    }
    LocalFree(argv);

    if (install || uninstall) {
        AttachConsoleIfAny(false);
        media::MfScope mf;
        std::wstring error;
        const bool ok = install ? agent::InstallCamera(dll, per_user, error) : agent::UninstallCamera(dll, per_user, error);
        if (ok) std::printf("Tethercam virtual camera %s.\n", install ? "installed" : "removed");
        else std::fprintf(stderr, "Failed: %s\n", Narrow(error).c_str());
        return ok ? 0 : 1;
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\Tethercam.Agent");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (console) AttachConsoleIfAny(false), std::fprintf(stderr, "Tethercam agent is already running.\n");
        return 0;
    }

    if (console) AttachConsoleIfAny(true);
    const std::wstring log_dir = LogDirectory();
    if (!log_dir.empty()) log::SetFile(log_dir + L"\\agent.log");

    media::MfScope mf;
    std::unique_ptr<agent::SessionCamera> session;
    if (session_camera) {
        session = std::make_unique<agent::SessionCamera>();
        std::wstring error;
        if (session->Create(dll, error)) log::Info("session camera created");
        else log::Error("session camera failed: {}", Narrow(error));
    }

    agent::Engine engine(options);
    agent::StatusServer status([&engine] { return engine.StatusJson(); });
    status.Start();
    int result = 0;
    if (console) {
        engine.Start([&status] { status.Broadcast(); });
        std::printf("Tethercam agent %.*s running. Press Ctrl+C to quit.\n", int(Version().size()), Version().data());
        static HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler([](DWORD) -> BOOL { return SetEvent(quit); }, TRUE);
        WaitForSingleObject(quit, INFINITE);
    } else {
        agent::Tray tray(instance, engine, log_dir);
        engine.Start([&tray, &status] {
            tray.PostRefresh();
            status.Broadcast();
        });
        if (preview) tray.ShowPreview();
        result = tray.Run();
    }
    engine.StopThread();
    status.Stop();
    session.reset();
    if (single) CloseHandle(single);
    return result;
}
