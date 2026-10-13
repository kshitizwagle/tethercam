#include "preview.h"

#include <string>

namespace tethercam::agent {
namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr uint64_t kStaleMs = 1000;

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
        RECT r{0, 0, 960, 540};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, L"TethercamPreview", L"Tethercam preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance_, this);
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
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (last_frame_ms_ && GetTickCount64() - last_frame_ms_ > kStaleMs) {
        last_frame_ms_ = 0;  // switch to the status text
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (!last_frame_ms_) {
        InvalidateRect(hwnd_, nullptr, FALSE);  // keep the status text current
    }
    const Snapshot s = engine_.Get();
    std::wstring title = L"Tethercam preview";
    if (!s.device_name.empty()) title += L" — " + Widen(s.device_name);
    if (!s.detail.empty()) title += L" — " + Widen(s.detail);
    SetWindowTextW(hwnd_, title.c_str());
}

void PreviewWindow::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT client;
    GetClientRect(hwnd_, &client);
    const int cw = client.right, ch = client.bottom;
    // Double-buffer to avoid flicker.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 22));
    FillRect(mem, &client, bg);
    DeleteObject(bg);
    if (last_frame_ms_ && width_ && height_) {
        int w = cw, h = int(int64_t(cw) * height_ / width_);
        if (h > ch) {
            h = ch;
            w = int(int64_t(ch) * width_ / height_);
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = int(width_);
        bi.bmiHeader.biHeight = -int(height_);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(mem, HALFTONE);
        StretchDIBits(mem, (cw - w) / 2, (ch - h) / 2, w, h, 0, 0, int(width_), int(height_), bgra_.data(), &bi,
                      DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(220, 220, 228));
        HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old_font = SelectObject(mem, font);
        RECT text = client;
        DrawTextW(mem, Hint(engine_.Get().status), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, old_font);
        DeleteObject(font);
    }
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
        case WM_CLOSE:
            self->Hide();  // keep the window object; reopening is instant
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace tethercam::agent
