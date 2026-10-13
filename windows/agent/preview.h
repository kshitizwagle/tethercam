#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wil/com.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "tethercam/image.h"

namespace tethercam::agent {

// Lightweight preview window (Win32 + Direct2D/DirectWrite, no extra runtime), opened from the tray.
// The video fills the window; Camera-app-style controls float over it and fade out when the mouse is
// idle. While it's visible the engine keeps the phone streaming (Engine::SetPreview).
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
        D2D1_RECT_F rect;
        Action action;
        int camera_id;
        std::wstring text;
        bool active;
        bool round;
    };
    struct Anim {
        float hover = 0, press = 0, active = 0;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Tick();
    void Render();
    bool CreateDeviceResources();
    void Layout(const Snapshot& s, float width, float height);
    void Click(float x, float y);
    const Button* HitTest(float x, float y) const;
    static int Key(const Button& b) { return int(b.action) * 64 + b.camera_id + 1; }

    HINSTANCE instance_;
    Engine& engine_;
    HWND hwnd_ = nullptr;

    wil::com_ptr<ID2D1Factory> d2d_;
    wil::com_ptr<IDWriteFactory> dwrite_;
    wil::com_ptr<ID2D1HwndRenderTarget> target_;
    wil::com_ptr<ID2D1Bitmap> video_;
    wil::com_ptr<ID2D1SolidColorBrush> brush_;
    wil::com_ptr<IDWriteTextFormat> label_font_, small_font_, icon_font_, hint_font_, hint_icon_font_;

    image::Nv12Frame frame_;
    std::vector<uint8_t> bgra_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t seq_ = 0;
    uint64_t last_frame_ms_ = 0;
    bool video_dirty_ = false;

    std::vector<Button> buttons_;
    std::map<int, Anim> anims_;
    float mouse_x_ = -1, mouse_y_ = -1;
    int pressed_key_ = -1;
    uint64_t last_mouse_ms_ = 0;
    float chrome_ = 1;      // opacity of the floating controls (fade out when idle)
    float thumb_x_ = -1;    // animated position of the selected-lens highlight
    uint64_t last_anim_ms_ = 0;
    int last_back_camera_ = -1;
    float dpi_scale_ = 1;
};

}  // namespace tethercam::agent
