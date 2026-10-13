#include "tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "tethercam/version.h"

namespace tethercam::agent {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kRefreshMessage = WM_APP + 2;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kQuit = 100,
    kPreview,
    kOpenLogs,
    kMirror,
    kRotate0,
    kRotate90,
    kRotate180,
    kRotate270,
    kCameraBase = 200,
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// A filled circle in the given colour; no icon resources needed.
HICON MakeDotIcon(COLORREF color, bool ring) {
    const int size = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color_bmp = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = static_cast<uint32_t*>(bits);
    const float c = (size - 1) / 2.0f, r = size * 0.42f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float d = std::hypot(x - c, y - c);
            float a = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            uint32_t rgb = (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
            if (ring && d < r * 0.45f) rgb = 0xFFFFFF;  // white center dot = streaming
            const uint32_t alpha = uint32_t(a * 255);
            const uint32_t pr = ((rgb >> 16) & 0xFF) * alpha / 255, pg = ((rgb >> 8) & 0xFF) * alpha / 255,
                           pb = (rgb & 0xFF) * alpha / 255;
            px[y * size + x] = (alpha << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color_bmp};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color_bmp);
    return icon;
}

const wchar_t* StatusText(shm::AgentStatus s) {
    switch (s) {
    case shm::AgentStatus::NoDevice: return L"Waiting for iPhone (USB)";
    case shm::AgentStatus::AppNotOpen: return L"Open Tethercam on your iPhone";
    case shm::AgentStatus::Connected: return L"Connected";
    case shm::AgentStatus::Streaming: return L"Streaming";
    case shm::AgentStatus::PausedOnPhone: return L"Paused on iPhone";
    case shm::AgentStatus::NoUsbmuxd: return L"Install Apple Devices (Microsoft Store)";
    default: return L"Starting";
    }
}

}  // namespace

Tray::Tray(HINSTANCE instance, Engine& engine, std::wstring log_dir)
    : instance_(instance), engine_(engine), log_dir_(std::move(log_dir)) {
    icons_[0] = MakeDotIcon(RGB(140, 140, 150), false);  // no device
    icons_[1] = MakeDotIcon(RGB(235, 160, 40), false);   // app not open / paused
    icons_[2] = MakeDotIcon(RGB(40, 170, 90), false);    // connected
    icons_[3] = MakeDotIcon(RGB(40, 170, 90), true);     // streaming
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"TethercamTray";
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Tethercam", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    Refresh();
}

Tray::~Tray() {
    if (added_) {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (hwnd_) DestroyWindow(hwnd_);
    for (HICON i : icons_)
        if (i) DestroyIcon(i);
}

int Tray::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void Tray::PostRefresh() { PostMessageW(hwnd_, kRefreshMessage, 0, 0); }

HICON Tray::IconFor(shm::AgentStatus s) {
    switch (s) {
    case shm::AgentStatus::Streaming: return icons_[3];
    case shm::AgentStatus::Connected: return icons_[2];
    case shm::AgentStatus::AppNotOpen:
    case shm::AgentStatus::PausedOnPhone:
    case shm::AgentStatus::NoUsbmuxd: return icons_[1];
    default: return icons_[0];
    }
}

void Tray::Refresh() {
    const Snapshot s = engine_.Get();
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = IconFor(s.status);
    std::wstring tip = std::wstring(L"Tethercam: ") + StatusText(s.status);
    if (!s.detail.empty()) tip += L"\n" + Widen(s.detail);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (!added_) added_ = Shell_NotifyIconW(NIM_ADD, &nid);
    else Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::ShowMenu() {
    const Snapshot s = engine_.Get();
    HMENU menu = CreatePopupMenu();
    std::wstring status = StatusText(s.status);
    if (!s.device_name.empty()) status += L": " + Widen(s.device_name);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_STRING, kPreview, L"Show preview");
    SetMenuDefaultItem(menu, kPreview, FALSE);
    if (!s.detail.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Widen(s.detail).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!s.cameras.empty()) {
        for (const auto& c : s.cameras) {
            const UINT flags = MF_STRING | (s.selected_camera == c.camera_id ? MF_CHECKED : 0);
            AppendMenuW(menu, flags, kCameraBase + c.camera_id, Widen(c.label).c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING | (s.mirror ? MF_CHECKED : 0), kMirror, L"Mirror");
    HMENU rotate = CreatePopupMenu();
    const std::pair<UINT, uint16_t> rotations[] = {
        {kRotate0, uint16_t(0)}, {kRotate90, uint16_t(90)}, {kRotate180, uint16_t(180)}, {kRotate270, uint16_t(270)}};
    for (auto [id, deg] : rotations)
        AppendMenuW(rotate, MF_STRING | (s.rotation == deg ? MF_CHECKED : 0), id, std::format(L"{}°", deg).c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rotate), L"Rotate");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kOpenLogs, L"Open log folder");
    const std::wstring about = L"Tethercam " + Widen(std::string(Version())) + L" (GPL-3.0)";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, about.c_str());
    AppendMenuW(menu, MF_STRING, kQuit, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == kQuit) PostQuitMessage(0);
    else if (cmd == kPreview) ShowPreview();
    else if (cmd == kOpenLogs) ShellExecuteW(nullptr, L"open", log_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == kMirror) engine_.SetMirror(!s.mirror);
    else if (cmd >= kRotate0 && cmd <= kRotate270) engine_.SetRotation(uint16_t((cmd - kRotate0) * 90));
    else if (cmd >= kCameraBase) engine_.SelectCamera(int(cmd - kCameraBase));
}

void Tray::ShowPreview() {
    if (!preview_) preview_ = std::make_unique<PreviewWindow>(instance_, engine_);
    preview_->Show();
}

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    }
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == kRefreshMessage) {
            self->Refresh();
            return 0;
        }
        if (msg == kTrayMessage && LOWORD(lp) == WM_RBUTTONUP) {
            self->ShowMenu();
            return 0;
        }
        if (msg == kTrayMessage && (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)) {
            self->ShowPreview();
            return 0;
        }
        if (msg == self->taskbar_created_) {  // Explorer restarted
            self->added_ = false;
            self->Refresh();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace tethercam::agent
