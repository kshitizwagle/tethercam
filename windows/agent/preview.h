#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "tethercam/image.h"

namespace tethercam::agent {

// A resizable window showing the live iPhone video, opened from the tray, with Camera-app-style
// controls along the bottom (lens, flip front/back, mirror, rotate). While it's visible the engine
// keeps the phone streaming (Engine::SetPreview).
class PreviewWindow {
public:
    PreviewWindow(HINSTANCE instance, Engine& engine);
    ~PreviewWindow();
    void Show();
    void Hide();
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

private:
    enum class Action { Lens, Flip, Mirror, Rotate };
    struct Button {
        RECT rect;
        Action action;
        int camera_id;
        std::wstring text;
        bool active;   // selected lens / mirror on
        bool round;    // circular icon button
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Tick();
    void Paint();
    void Layout(const Snapshot& s, int width, int top);
    void Click(POINT pt);
    const Button* HitTest(POINT pt) const;
    static int Key(const Button& b) { return int(b.action) * 64 + b.camera_id + 1; }
    void Animate();

    HINSTANCE instance_;
    Engine& engine_;
    HWND hwnd_ = nullptr;
    image::Nv12Frame frame_;
    std::vector<uint8_t> bgra_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t seq_ = 0;
    uint64_t last_frame_ms_ = 0;
    std::vector<Button> buttons_;
    POINT mouse_{-1, -1};  // last cursor position, for hover highlight
    int pressed_key_ = -1;  // button under the mouse while the left button is down
    // Per-button animation state (0..1), eased toward its target every tick.
    struct Anim {
        float hover = 0, press = 0, active = 0;
    };
    std::map<int, Anim> anims_;
    uint64_t last_anim_ms_ = 0;
    int last_back_camera_ = -1;
};

}  // namespace tethercam::agent
