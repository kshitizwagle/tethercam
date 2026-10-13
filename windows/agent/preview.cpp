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
constexpr uint64_t kIdleHideMs = 2500;  // controls fade out after this long without mouse movement
constexpr float kTauMs = 60.0f;         // ease-out time constant: ~180 ms to settle

// Segoe Fluent Icons (Windows 11).
constexpr wchar_t kIconFlip[] = L"";
constexpr wchar_t kIconRotate[] = L"";
constexpr wchar_t kIconMirror[] = L"";
constexpr wchar_t kIconCamera[] = L"";

constexpr uint32_t kYellow = 0xFFD60A;  // iOS Camera accent
constexpr uint32_t kRed = 0xFF3B30;

D2D1_COLOR_F Rgba(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
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
    case shm::AgentStatus::NoUsbmuxd: return L"Install Apple Devices from the Microsoft Store";
    case shm::AgentStatus::NoDevice: return L"Connect your iPhone with a USB cable";
    case shm::AgentStatus::AppNotOpen: return L"Open Tethercam on your iPhone";
    case shm::AgentStatus::PausedOnPhone: return L"Paused — bring Tethercam to the front on your iPhone";
    default: return L"Starting the iPhone camera…";
    }
}

// Camera-app zoom name: ".5×", "1×", "5×" (the phone appends the factor to the label).
std::wstring LensText(const proto::Camera& c) {
    const std::wstring label = Widen(c.label);
    const auto space = label.find_last_of(L' ');
    std::wstring last = space == std::wstring::npos ? label : label.substr(space + 1);
    if (last.empty() || last.back() != L'×')
        last = c.lens == proto::Lens::UltraWide ? L"0.5×" : c.lens == proto::Lens::Telephoto ? L"2×" : L"1×";
    if (last.rfind(L"0.", 0) == 0) last = last.substr(1);
    return last;
}

wil::com_ptr<IDWriteTextFormat> MakeFormat(IDWriteFactory* f, const wchar_t* family, float size, DWRITE_FONT_WEIGHT w) {
    wil::com_ptr<IDWriteTextFormat> fmt;
    if (FAILED(f->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                   L"en-us", fmt.put())))
        f->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                            L"en-us", fmt.put());
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return fmt;
}

}  // namespace

PreviewWindow::PreviewWindow(HINSTANCE instance, Engine& engine) : instance_(instance), engine_(engine) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite_.put()));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"TethercamPreview";
    RegisterClassExW(&wc);
}

PreviewWindow::~PreviewWindow() {
    engine_.SetPreview(false);
    if (hwnd_) DestroyWindow(hwnd_);
}

void PreviewWindow::Show() {
    if (!hwnd_) {
        const UINT dpi = GetDpiForSystem();
        RECT r{0, 0, LONG(960 * dpi / 96), LONG(540 * dpi / 96)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        hwnd_ = CreateWindowExW(0, L"TethercamPreview", L"Tethercam", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance_, this);
        // Windows 11 look: dark Mica title bar and rounded corners.
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        int backdrop = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
        int corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    }
    engine_.SetPreview(true);
    last_mouse_ms_ = GetTickCount64();
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

bool PreviewWindow::CreateDeviceResources() {
    if (target_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(d2d_->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), target_.put())))
        return false;
    // Draw in DIPs so the controls scale with the display's DPI.
    const UINT dpi = GetDpiForWindow(hwnd_);
    target_->SetDpi(float(dpi), float(dpi));
    dpi_scale_ = dpi / 96.0f;
    target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush_.put());
    label_font_ = MakeFormat(dwrite_.get(), L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    small_font_ = MakeFormat(dwrite_.get(), L"Segoe UI Variable Text", 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    icon_font_ = MakeFormat(dwrite_.get(), L"Segoe Fluent Icons", 17, DWRITE_FONT_WEIGHT_NORMAL);
    hint_font_ = MakeFormat(dwrite_.get(), L"Segoe UI Variable Display", 17, DWRITE_FONT_WEIGHT_NORMAL);
    hint_icon_font_ = MakeFormat(dwrite_.get(), L"Segoe Fluent Icons", 40, DWRITE_FONT_WEIGHT_NORMAL);
    video_.reset();
    video_dirty_ = true;
    return true;
}

void PreviewWindow::Tick() {
    if (engine_.CopyPreview(frame_, seq_)) {
        width_ = frame_.width;
        height_ = frame_.height;
        bgra_.resize(size_t(width_) * height_ * 4);
        image::Nv12ToBgra(frame_.View(), bgra_.data(), width_ * 4);
        last_frame_ms_ = GetTickCount64();
        video_dirty_ = true;
    } else if (last_frame_ms_ && GetTickCount64() - last_frame_ms_ > kStaleMs) {
        last_frame_ms_ = 0;
    }
    const Snapshot s = engine_.Get();
    std::wstring title = L"Tethercam";
    if (!s.device_name.empty()) title += L" — " + Widen(s.device_name);
    SetWindowTextW(hwnd_, title.c_str());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewWindow::Layout(const Snapshot& s, float w, float h) {
    buttons_.clear();
    const proto::Camera* current = nullptr;
    for (const auto& c : s.cameras)
        if (c.camera_id == s.selected_camera) current = &c;
    if (!current && !s.cameras.empty()) current = &s.cameras.front();
    if (current && current->position == proto::Position::Back) last_back_camera_ = current->camera_id;

    const float cy = h - 44;
    // Centre: lens selector for the back cameras.
    std::vector<const proto::Camera*> lenses;
    if (current && current->position == proto::Position::Back)
        for (const auto& c : s.cameras)
            if (c.position == proto::Position::Back) lenses.push_back(&c);
    auto order = [](const proto::Camera* c) {
        return c->lens == proto::Lens::UltraWide ? 0 : c->lens == proto::Lens::Wide ? 1 : 2;
    };
    std::sort(lenses.begin(), lenses.end(), [&](auto* a, auto* b) { return order(a) < order(b); });
    if (lenses.size() > 1) {
        const float pill = 40, gap = 4;
        const float total = lenses.size() * pill + (lenses.size() - 1) * gap;
        float x = w / 2 - total / 2;
        for (auto* c : lenses) {
            buttons_.push_back({D2D1::RectF(x, cy - pill / 2, x + pill, cy + pill / 2), Action::Lens, c->camera_id,
                                LensText(*c), c == current, false});
            x += pill + gap;
        }
    }
    // Quality has its own row below the live chip, clear of lens and framing controls.
    buttons_.push_back({D2D1::RectF(16, 54, 246, 98), Action::Quality, -1,
                        L"Quality: " + Widen(QualityLabel(s.quality)), false, false});
    // Left: mirror, rotate. Right: switch camera.
    const float r = 22;
    buttons_.push_back({D2D1::RectF(24, cy - r, 24 + 2 * r, cy + r), Action::Mirror, -1, kIconMirror, s.mirror, true});
    buttons_.push_back({D2D1::RectF(24 + 2 * r + 12, cy - r, 24 + 4 * r + 12, cy + r), Action::Rotate, -1, kIconRotate,
                        s.rotation != 0, true});
    bool has_front = false, has_back = false;
    for (const auto& c : s.cameras) (c.position == proto::Position::Front ? has_front : has_back) = true;
    if (has_front && has_back)
        buttons_.push_back(
            {D2D1::RectF(w - 24 - 2 * r, cy - r, w - 24, cy + r), Action::Flip, -1, kIconFlip, false, true});
}

const PreviewWindow::Button* PreviewWindow::HitTest(float x, float y) const {
    if (chrome_ < 0.2f) return nullptr;  // hidden controls aren't clickable
    for (const auto& b : buttons_)
        if (x >= b.rect.left && x <= b.rect.right && y >= b.rect.top && y <= b.rect.bottom) return &b;
    return nullptr;
}

void PreviewWindow::Click(float x, float y) {
    const Button* b = HitTest(x, y);
    if (!b) return;
    const Snapshot s = engine_.Get();
    switch (b->action) {
    case Action::Quality:
        ShowQualityMenu();
        break;
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
}

void PreviewWindow::ShowQualityMenu() {
    const auto s = engine_.Get();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Capture quality (video apps choose output size)");
    const proto::Camera* camera = nullptr;
    for (const auto& c : s.cameras)
        if (c.camera_id == s.selected_camera) camera = &c;
    for (UINT i = 0; i <= 4; ++i) {
        const auto q = proto::Quality(i);
        const bool supported = !camera || SupportsQuality(camera->formats, q);
        const std::wstring label = i == 0 ? L"Auto (match video app)" : Widen(QualityLabel(q));
        AppendMenuW(menu, MF_STRING | (s.quality == q ? MF_CHECKED : 0) | (supported ? 0 : MF_GRAYED),
                    i + 1, label.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"4K uses more power and can warm your phone");
    POINT pt{LONG(16 * dpi_scale_), LONG(98 * dpi_scale_)};
    ClientToScreen(hwnd_, &pt);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= 5) engine_.SetQuality(proto::Quality(cmd - 1));
    last_mouse_ms_ = GetTickCount64();
}

void PreviewWindow::Render() {
    if (!CreateDeviceResources()) return;
    const D2D1_SIZE_F size = target_->GetSize();
    const float w = size.width, h = size.height;
    const Snapshot s = engine_.Get();
    const bool live = last_frame_ms_ != 0 && width_ && height_;

    // Animation step (exponential ease-out, frame-rate independent).
    const uint64_t now = GetTickCount64();
    const float dt = last_anim_ms_ ? float(std::min<uint64_t>(now - last_anim_ms_, 100)) : 16.0f;
    last_anim_ms_ = now;
    const float k = 1.0f - std::exp(-dt / kTauMs);
    const bool show_chrome = !live || now - last_mouse_ms_ < kIdleHideMs;
    chrome_ += ((show_chrome ? 1.0f : 0.0f) - chrome_) * (show_chrome ? k : k * 0.5f);

    Layout(s, w, h);
    for (const auto& b : buttons_) {
        Anim& a = anims_[Key(b)];
        const bool over = mouse_x_ >= b.rect.left && mouse_x_ <= b.rect.right && mouse_y_ >= b.rect.top &&
                          mouse_y_ <= b.rect.bottom;
        a.hover += ((over ? 1.0f : 0.0f) - a.hover) * k;
        a.press += ((pressed_key_ == Key(b) && over ? 1.0f : 0.0f) - a.press) * std::min(1.0f, 2 * k);
        a.active += ((b.active ? 1.0f : 0.0f) - a.active) * k;
        if (b.action == Action::Lens && b.active) {
            const float cx = (b.rect.left + b.rect.right) / 2;
            thumb_x_ = thumb_x_ < 0 ? cx : thumb_x_ + (cx - thumb_x_) * k;
        }
    }

    target_->BeginDraw();
    target_->Clear(Rgba(0x000000));

    // Video, fitted to the window.
    if (live) {
        if (video_dirty_) {
            const auto bsize = video_ ? video_->GetPixelSize() : D2D1_SIZE_U{};
            if (!video_ || bsize.width != width_ || bsize.height != height_) {
                video_.reset();
                target_->CreateBitmap(D2D1::SizeU(width_, height_), bgra_.data(), width_ * 4,
                                      D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                               D2D1_ALPHA_MODE_IGNORE)),
                                      video_.put());
            } else {
                video_->CopyFromMemory(nullptr, bgra_.data(), width_ * 4);
            }
            video_dirty_ = false;
        }
        float vw = w, vh = w * height_ / width_;
        if (vh > h) {
            vh = h;
            vw = h * width_ / height_;
        }
        if (video_)
            target_->DrawBitmap(video_.get(), D2D1::RectF((w - vw) / 2, (h - vh) / 2, (w + vw) / 2, (h + vh) / 2),
                                1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        brush_->SetColor(Rgba(0xFFFFFF, 0.35f));
        target_->DrawTextW(kIconCamera, 1, hint_icon_font_.get(), D2D1::RectF(0, h / 2 - 70, w, h / 2 - 10),
                           brush_.get());
        brush_->SetColor(Rgba(0xFFFFFF, 0.85f));
        const wchar_t* hint = Hint(s.status);
        target_->DrawTextW(hint, UINT32(wcslen(hint)), hint_font_.get(), D2D1::RectF(0, h / 2, w, h / 2 + 30),
                           brush_.get());
    }

    // Bottom scrim so the controls stay readable over bright video.
    if (live && chrome_ > 0.01f) {
        D2D1_GRADIENT_STOP stops[] = {{0.0f, Rgba(0x000000, 0.0f)}, {1.0f, Rgba(0x000000, 0.55f * chrome_)}};
        wil::com_ptr<ID2D1GradientStopCollection> coll;
        target_->CreateGradientStopCollection(stops, 2, coll.put());
        wil::com_ptr<ID2D1LinearGradientBrush> grad;
        target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, h - 130), D2D1::Point2F(0, h)), coll.get(),
            grad.put());
        target_->FillRectangle(D2D1::RectF(0, h - 130, w, h), grad.get());
    }

    // LIVE chip, top left (like the Camera app's recording timer).
    if (s.status == shm::AgentStatus::Streaming && live && chrome_ > 0.01f) {
        const std::wstring text = L"LIVE  " + Widen(s.detail);
        wil::com_ptr<IDWriteTextLayout> layout;
        dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), small_font_.get(), 1000, 24, layout.put());
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const D2D1_RECT_F chip = D2D1::RectF(16, 16, 16 + m.width + 34, 42);
        brush_->SetColor(Rgba(kRed, 0.92f * chrome_));
        target_->FillRoundedRectangle(D2D1::RoundedRect(chip, 6, 6), brush_.get());
        brush_->SetColor(Rgba(0xFFFFFF, chrome_));
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(chip.left + 12, (chip.top + chip.bottom) / 2), 3.5f, 3.5f),
                             brush_.get());
        target_->DrawTextLayout(D2D1::Point2F(chip.left + 22, chip.top + 1), layout.get(), brush_.get());
    }

    // Controls.
    if (chrome_ > 0.01f) {
        // Lens capsule with a highlight that slides to the selected lens.
        std::vector<const Button*> pills;
        for (const auto& b : buttons_)
            if (b.action == Action::Lens) pills.push_back(&b);
        if (!pills.empty()) {
            const D2D1_RECT_F cap = D2D1::RectF(pills.front()->rect.left - 4, pills.front()->rect.top - 4,
                                                pills.back()->rect.right + 4, pills.back()->rect.bottom + 4);
            brush_->SetColor(Rgba(0x000000, 0.45f * chrome_));
            target_->FillRoundedRectangle(D2D1::RoundedRect(cap, 24, 24), brush_.get());
            if (thumb_x_ >= 0) {
                const float cy = (pills.front()->rect.top + pills.front()->rect.bottom) / 2;
                brush_->SetColor(Rgba(0x2C2C2E, 0.95f * chrome_));
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x_, cy), 20, 20), brush_.get());
            }
        }
        for (const auto& b : buttons_) {
            const Anim a = anims_[Key(b)];
            const float scale = 1.0f - 0.06f * a.press;
            const float cx = (b.rect.left + b.rect.right) / 2, cy = (b.rect.top + b.rect.bottom) / 2;
            const float rx = (b.rect.right - b.rect.left) / 2 * scale, ry = (b.rect.bottom - b.rect.top) / 2 * scale;
            const D2D1_RECT_F box = D2D1::RectF(cx - rx, cy - ry, cx + rx, cy + ry);
            if (b.action == Action::Quality) {
                brush_->SetColor(Rgba(0x1C1C1E, 0.92f * chrome_));
                target_->FillRoundedRectangle(D2D1::RoundedRect(box, 6, 6), brush_.get());
                brush_->SetColor(Rgba(0xFFFFFF, chrome_));
                target_->DrawTextW(b.text.c_str(), UINT32(b.text.size()), small_font_.get(), box, brush_.get());
            } else if (b.round) {
                // Glass button: translucent fill, brighter when on or hovered.
                D2D1_COLOR_F fill = Mix(Rgba(0x3A3A3C, 0.55f), Rgba(0x8E8E93, 0.65f), a.active);
                fill = Mix(fill, Rgba(0x8E8E93, 0.75f), a.hover * 0.5f);
                fill.a *= chrome_;
                brush_->SetColor(fill);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush_.get());
                brush_->SetColor(Rgba(0xFFFFFF, chrome_));
                target_->DrawTextW(b.text.c_str(), UINT32(b.text.size()), icon_font_.get(), box, brush_.get());
            } else {
                // Selected lens: yellow with the "×"; others: white number only, like the Camera app.
                D2D1_COLOR_F color = Mix(Rgba(0xFFFFFF), Rgba(kYellow), a.active);
                color.a = chrome_ * (0.8f + 0.2f * std::max(a.active, a.hover));
                brush_->SetColor(color);
                const bool selected = a.active > 0.5f;
                const std::wstring text = selected ? b.text : b.text.substr(0, b.text.size() - 1);
                target_->DrawTextW(text.c_str(), UINT32(text.size()), selected ? label_font_.get() : small_font_.get(),
                                   box, brush_.get());
            }
        }
    }

    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        target_.reset();
        video_.reset();
    }
}

LRESULT CALLBACK PreviewWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<PreviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        auto dip = [self](LPARAM l, float& x, float& y) {
            x = GET_X_LPARAM(l) / self->dpi_scale_;
            y = GET_Y_LPARAM(l) / self->dpi_scale_;
        };
        switch (msg) {
        case WM_KEYDOWN:
            if (wp == 'Q' || wp == VK_F10) {
                self->ShowQualityMenu();
                return 0;
            }
            break;
        case WM_TIMER:
            self->Tick();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            self->Render();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (self->target_) self->target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            return 0;
        case WM_DPICHANGED: {
            self->target_.reset();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            dip(lp, self->mouse_x_, self->mouse_y_);
            self->last_mouse_ms_ = GetTickCount64();
            SetCursor(LoadCursorW(nullptr, self->HitTest(self->mouse_x_, self->mouse_y_) ? IDC_HAND : IDC_ARROW));
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            self->mouse_x_ = self->mouse_y_ = -1;
            self->pressed_key_ = -1;
            return 0;
        case WM_LBUTTONDOWN: {
            float x, y;
            dip(lp, x, y);
            const Button* b = self->HitTest(x, y);
            self->pressed_key_ = b ? Key(*b) : -1;
            self->last_mouse_ms_ = GetTickCount64();
            return 0;
        }
        case WM_LBUTTONUP: {
            float x, y;
            dip(lp, x, y);
            self->Click(x, y);
            self->pressed_key_ = -1;
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = POINT{480, 320};
            return 0;
        case WM_CLOSE:
            self->Hide();  // keep the window; reopening is instant
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace tethercam::agent
