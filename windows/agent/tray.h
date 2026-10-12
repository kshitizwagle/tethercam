#pragma once

#include <windows.h>

#include <string>

#include "engine.h"

namespace tethercam::agent {

// Notification-area icon and menu (PRODUCT.md "Windows tray menu").
class Tray {
public:
    Tray(HINSTANCE instance, Engine& engine, std::wstring log_dir);
    ~Tray();
    int Run();
    // Thread-safe: asks the UI thread to refresh icon and tooltip.
    void PostRefresh();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Refresh();
    void ShowMenu();
    HICON IconFor(shm::AgentStatus s);

    HINSTANCE instance_;
    Engine& engine_;
    std::wstring log_dir_;
    HWND hwnd_ = nullptr;
    HICON icons_[4] = {};
    bool added_ = false;
    UINT taskbar_created_ = 0;
};

}  // namespace tethercam::agent
