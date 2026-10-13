#include "preview.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace tethercam::agent {
namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr uint64_t kStaleMs = 1000;
constexpr int kBarHeight = 72;
constexpr COLORREF kYellow = RGB(255, 214, 10);  // iOS Camera accent
// Segoe Fluent Icons (Windows 11): switch camera, rotate, mirror.
constexpr wchar_t kIconFlip[] = L"\uE89E";
constexpr wchar_t kIconRotate[] = L"\uE8B4";
constexpr wchar_t kIconMirror[] = L"\uE8AB";
// Ease-out time constant: ~3 tau (180 ms) to settle, short enough to feel immediate.
constexpr float kTauMs = 60.0f;

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    auto ch = [t](int x, int y) { return int(x + (y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

const wchar_t* Hint(shm::AgentStatus s) {
    switch (s) {
    case shm::AgentStatus::NoUsbmuxd: return L"Install Apple Devices from the Microsoft Store.";
    case shm::AgentStatus::NoDevice: return L"Connect your iPhone with a USB cable.";
    case shm::AgentStatus::AppNotOpen: return L"Open Tethercam on your iPhone (unlock it and tap Trust if asked).";
    case shm::AgentStatus::PausedOnPhone: return L"Paused on iPhone: bring the Tethercam app to the front.";
    default: return L"Starting the iPhone camera…";
    }
}

// Zoom-style name for a lens, like the iOS Camera app: "0.5×", "1×", "5×".
std::wstring LensText(const proto::Camera& c) {
    const std::wstring label = Widen(c.label);
    const auto space = label.find_last_of(L' ');
    const std::wstring last = space == std::wstring::npos ? label : label.substr(space + 1);
    if (!last.empty() && last.back() == L'×') return last;  // the phone sent a zoom factor
    switch (c.lens) {
    case proto::Lens::UltraWide: return L"0.5×";
    case proto::Lens::Wide: return L"1×";
    case proto::Lens::Telephoto: return L"Tele";
    default: return L"1×";
    }
}

HFONT MakeFont(int px, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

}  // namespace

PreviewWindow::PreviewWindow(HINSTANCE instance, Engine& engine) : instance_(instance), engine_(engine) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // we paint everything
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"TethercamPreview";
    RegisterClassExW(&wc);
}

PreviewWindow::~PreviewWindow() {
    engine_.SetPreview(false);
    if (hwnd_) DestroyWindow(hwnd_);
}

void PreviewWindow::Show() {
    if (!hwnd_) {
        RECT r{0, 0, 960, 540 + kBarHeight};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, L"TethercamPreview", L"Tethercam preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance_, this);
        BOOL dark = TRUE;  // dark title bar to match the black UI
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    engine_.SetPreview(true);
    SetTimer(hwnd_, kTimerId, 15, nullptr);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void PreviewWindow::Hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimerId);
    ShowWindow(hwnd_, SW_HIDE);
    engine_.SetPreview(false);
}

void PreviewWindow::Tick() {
    if (engine_.CopyPreview(frame_, seq_)) {
        width_ = frame_.width;
        height_ = frame_.height;
        bgra_.resize(size_t(width_) * height_ * 4);
        image::Nv12ToBgra(frame_.View(), bgra_.data(), width_ * 4);
        last_frame_ms_ = GetTickCount64();
    } else if (last_frame_ms_ && GetTickCount64() - last_frame_ms_ > kStaleMs) {
        last_frame_ms_ = 0;  // switch to the status text
    }
    Animate();
    InvalidateRect(hwnd_, nullptr, FALSE);
    const Snapshot s = engine_.Get();
    std::wstring title = L"Tethercam preview";
    if (!s.device_name.empty()) title += L" — " + Widen(s.device_name);
    if (!s.detail.empty()) title += L" — " + Widen(s.detail);
    SetWindowTextW(hwnd_, title.c_str());
}

void PreviewWindow::Layout(const Snapshot& s, int width, int top) {
    buttons_.clear();
    const proto::Camera* current = nullptr;
    for (const auto& c : s.cameras)
        if (c.camera_id == s.selected_camera) current = &c;
    if (!current && !s.cameras.empty()) current = &s.cameras.front();
    if (current && current->position == proto::Position::Back) last_back_camera_ = current->camera_id;

    const int cy = top + kBarHeight / 2;
    // Centre: lens pills for the side in use (like the Camera app's zoom buttons).
    std::vector<const proto::Camera*> lenses;
    if (current)
        for (const auto& c : s.cameras)
            if (c.position == current->position) lenses.push_back(&c);
    auto order = [](const proto::Camera* c) {  // 0.5x, 1x, tele
        return c->lens == proto::Lens::UltraWide ? 0 : c->lens == proto::Lens::Wide ? 1 : 2;
    };
    std::sort(lenses.begin(), lenses.end(), [&](auto* a, auto* b) { return order(a) < order(b); });
    const int pill = 52, gap = 8;
    const int total = int(lenses.size()) * pill + std::max(0, int(lenses.size()) - 1) * gap;
    int x = width / 2 - total / 2;
    if (lenses.size() > 1) {
        for (auto* c : lenses) {
            buttons_.push_back({RECT{x, cy - 18, x + pill, cy + 18}, Action::Lens, c->camera_id, LensText(*c),
                                current && c->camera_id == current->camera_id, false});
            x += pill + gap;
        }
    }
    // Left: mirror and rotate. Right: flip front/back.
    const int r = 22;
    buttons_.push_back({RECT{24, cy - r, 24 + 2 * r, cy + r}, Action::Mirror, -1, kIconMirror, s.mirror, true});
    buttons_.push_back({RECT{24 + 2 * r + 14, cy - r, 24 + 4 * r + 14, cy + r}, Action::Rotate, -1,
                        kIconRotate, s.rotation != 0, true});
    bool has_front = false, has_back = false;
    for (const auto& c : s.cameras) (c.position == proto::Position::Front ? has_front : has_back) = true;
    if (has_front && has_back)
        buttons_.push_back(
            {RECT{width - 24 - 2 * r, cy - r, width - 24, cy + r}, Action::Flip, -1, kIconFlip, false, true});
}

void PreviewWindow::Animate() {
    const uint64_t now = GetTickCount64();
    const float dt = last_anim_ms_ ? float(std::min<uint64_t>(now - last_anim_ms_, 100)) : 16.0f;
    last_anim_ms_ = now;
    const float k = 1.0f - std::exp(-dt / kTauMs);
    for (const auto& b : buttons_) {
        Anim& a = anims_[Key(b)];
        const bool over = PtInRect(&b.rect, mouse_) != FALSE;
        a.hover += ((over ? 1.0f : 0.0f) - a.hover) * k;
        a.press += ((pressed_key_ == Key(b) && over ? 1.0f : 0.0f) - a.press) * std::min(1.0f, k * 2);
        a.active += ((b.active ? 1.0f : 0.0f) - a.active) * k;
    }
}

const PreviewWindow::Button* PreviewWindow::HitTest(POINT pt) const {
    for (const auto& b : buttons_)
        if (PtInRect(&b.rect, pt)) return &b;
    return nullptr;
}

void PreviewWindow::Click(POINT pt) {
    const Button* b = HitTest(pt);
    if (!b) return;
    const Snapshot s = engine_.Get();
    switch (b->action) {
    case Action::Lens:
        engine_.SelectCamera(b->camera_id);
        break;
    case Action::Mirror:
        engine_.SetMirror(!s.mirror);
        break;
    case Action::Rotate:
        engine_.SetRotation(uint16_t((s.rotation + 90) % 360));
        break;
    case Action::Flip: {
        bool on_front = false;
        for (const auto& c : s.cameras)
            if (c.camera_id == s.selected_camera) on_front = c.position == proto::Position::Front;
        int target = -1;
        for (const auto& c : s.cameras) {
            if (on_front && c.position == proto::Position::Back &&
                (c.camera_id == last_back_camera_ || (target < 0 && c.lens == proto::Lens::Wide)))
                target = c.camera_id;
            if (!on_front && c.position == proto::Position::Front) target = c.camera_id;
        }
        if (target >= 0) engine_.SelectCamera(target);
        break;
    }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewWindow::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT client;
    GetClientRect(hwnd_, &client);
    const int cw = client.right, ch = client.bottom;
    const int vh = std::max(0, ch - kBarHeight);
    // Double-buffer to avoid flicker.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem, &client, black);
    DeleteObject(black);
    SetBkMode(mem, TRANSPARENT);

    // Video.
    if (last_frame_ms_ && width_ && height_ && vh > 0) {
        int w = cw, h = int(int64_t(cw) * height_ / width_);
        if (h > vh) {
            h = vh;
            w = int(int64_t(vh) * width_ / height_);
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = int(width_);
        bi.bmiHeader.biHeight = -int(height_);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(mem, HALFTONE);
        StretchDIBits(mem, (cw - w) / 2, (vh - h) / 2, w, h, 0, 0, int(width_), int(height_), bgra_.data(), &bi,
                      DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetTextColor(mem, RGB(220, 220, 228));
        HFONT font = MakeFont(18, FW_NORMAL);
        HGDIOBJ old_font = SelectObject(mem, font);
        RECT text{0, 0, cw, vh};
        DrawTextW(mem, Hint(engine_.Get().status), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, old_font);
        DeleteObject(font);
    }

    // Control bar.
    Layout(engine_.Get(), cw, vh);
    HFONT pill_font = MakeFont(15, FW_SEMIBOLD);
    HFONT icon_font = MakeFont(18, FW_NORMAL, L"Segoe Fluent Icons");
    HPEN no_pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ old_pen = SelectObject(mem, no_pen);
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const auto& b = buttons_[i];
        const Anim a = anims_[Key(b)];
        // Round buttons light up when on (mirror/rotate); pills turn their label yellow when selected.
        COLORREF fill = b.round ? Mix(RGB(38, 38, 42), RGB(84, 84, 90), a.active) : Mix(RGB(26, 26, 28), RGB(44, 44, 46), a.active);
        fill = Mix(fill, RGB(110, 110, 116), a.hover * 0.35f);
        // Pressed buttons shrink slightly (scale 0.94), anchored at their centre.
        const float scale = 1.0f - 0.06f * a.press;
        const float cx = (b.rect.left + b.rect.right) / 2.0f, cy = (b.rect.top + b.rect.bottom) / 2.0f;
        const float hw = (b.rect.right - b.rect.left) * scale / 2, hh = (b.rect.bottom - b.rect.top) * scale / 2;
        RECT r{int(cx - hw), int(cy - hh), int(cx + hw), int(cy + hh)};
        HBRUSH brush = CreateSolidBrush(fill);
        HGDIOBJ old_brush = SelectObject(mem, brush);
        if (b.round) Ellipse(mem, r.left, r.top, r.right, r.bottom);
        else RoundRect(mem, r.left, r.top, r.right, r.bottom, int(36 * scale), int(36 * scale));
        SelectObject(mem, old_brush);
        DeleteObject(brush);
        SelectObject(mem, b.round ? icon_font : pill_font);
        SetTextColor(mem, b.round ? RGB(255, 255, 255) : Mix(RGB(255, 255, 255), kYellow, a.active));
        RECT t = r;
        DrawTextW(mem, b.text.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(mem, old_pen);
    DeleteObject(no_pen);
    DeleteObject(pill_font);
    DeleteObject(icon_font);

    BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK PreviewWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<PreviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (msg) {
        case WM_TIMER:
            self->Tick();
            return 0;
        case WM_PAINT:
            self->Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Button* b = self->HitTest(pt);
            self->mouse_ = pt;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            SetCursor(LoadCursorW(nullptr, b ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            self->mouse_ = POINT{-1, -1};
            self->pressed_key_ = -1;
            return 0;
        case WM_LBUTTONDOWN: {
            const Button* b = self->HitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            self->pressed_key_ = b ? Key(*b) : -1;
            return 0;
        }
        case WM_LBUTTONUP:
            self->Click(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            self->pressed_key_ = -1;
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = POINT{480, 360};
            return 0;
        case WM_CLOSE:
            self->Hide();  // keep the window object; reopening is instant
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace tethercam::agent
