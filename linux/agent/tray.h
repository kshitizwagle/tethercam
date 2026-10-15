#pragma once

// Tray icon through StatusNotifierItem (AyatanaAppIndicator): status, lens, quality, mirror, rotate,
// log folder, quit. Same menu as the Windows tray (docs/PRODUCT.md). Runs the GTK main loop.

#include <filesystem>
#include <string>

#include "engine.h"

namespace tethercam::agent {

class Tray {
public:
    Tray(Engine& engine, std::filesystem::path log_dir) : engine_(engine), log_dir_(std::move(log_dir)) {}

    // Initializes GTK. False when there's no display (then run headless).
    static bool Available(int* argc, char*** argv);
    // Creates the icon; call before Run().
    void Create();
    // Thread-safe: schedules a refresh on the GTK thread.
    void Refresh();
    // Runs until Quit (menu, SIGINT or SIGTERM).
    void Run();

private:
    void Rebuild();
    static int OnIdle(void* self);

    Engine& engine_;
    std::filesystem::path log_dir_;
    void* indicator_ = nullptr;
    std::string last_key_;
    bool building_ = false;
};

}  // namespace tethercam::agent
