#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "engine.h"
#include "tethercam/image.h"

namespace tethercam::agent {

// A resizable window showing the live iPhone video, opened from the tray. While it's visible the
// engine keeps the phone streaming (Engine::SetPreview).
class PreviewWindow {
public:
    PreviewWindow(HINSTANCE instance, Engine& engine);
    ~PreviewWindow();
    void Show();
    void Hide();
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Tick();
    void Paint();

    HINSTANCE instance_;
    Engine& engine_;
    HWND hwnd_ = nullptr;
    image::Nv12Frame frame_;
    std::vector<uint8_t> bgra_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t seq_ = 0;
    uint64_t last_frame_ms_ = 0;
};

}  // namespace tethercam::agent
