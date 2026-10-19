#include "tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

#include "tethercam/version.h"

namespace tethercam::agent {
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kRefreshMessage = WM_APP + 2;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kQuit = 100,
    kPreview,
    kStatusCard,
    kOpenLogs,
    kMirror,
    kRotate0,
    kRotate90,
    kRotate180,
    kRotate270,
    kWifiAddress,
    kWifiForget,
    kCameraBase = 200,
    kQualityBase = 500,
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), ' ');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A one-line text prompt built from an in-memory dialog template (the agent has no .rc dialogs).
class TextPrompt {
public:
    static bool Ask(HWND owner, const wchar_t* title, const wchar_t* prompt, std::wstring& value) {
        TextPrompt self;
        self.value = &value;
        std::vector<WORD> t;
        auto align = [&] {
            if (t.size() % 2) t.push_back(0);  // DLGITEMTEMPLATEs start on DWORD boundaries
        };
        auto str = [&](const wchar_t* s) {
            for (; *s; ++s) t.push_back(WORD(*s));
            t.push_back(0);
        };
        auto dword = [&](DWORD v) {
            t.push_back(LOWORD(v));
            t.push_back(HIWORD(v));
        };
        auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
            align();
            dword(style | WS_CHILD | WS_VISIBLE);
            dword(0);
            for (short v : {x, y, cx, cy}) t.push_back(WORD(v));
            t.push_back(id);
            t.push_back(0xFFFF);
            t.push_back(cls);
            str(text);
            t.push_back(0);  // no creation data
        };
        dword(DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
        dword(0);
        t.push_back(4);  // items
        for (short v : {short(0), short(0), short(240), short(70)}) t.push_back(WORD(v));
        t.push_back(0);  // no menu
        t.push_back(0);  // default class
        str(title);
        t.push_back(9);
        str(L"Segoe UI");
        item(SS_LEFT, 8, 8, 224, 18, 100, 0x0082, prompt);
        item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 28, 224, 14, 101, 0x0081, L"");
        item(BS_DEFPUSHBUTTON | WS_TABSTOP, 126, 50, 50, 14, IDOK, 0x0080, L"OK");
        item(BS_PUSHBUTTON | WS_TABSTOP, 182, 50, 50, 14, IDCANCEL, 0x0080, L"Cancel");
        return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.data()), owner,
                                       &TextPrompt::Proc, reinterpret_cast<LPARAM>(&self)) == IDOK;
    }

private:
    std::wstring* value = nullptr;

    static INT_PTR CALLBACK Proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<TextPrompt*>(GetWindowLongPtrW(dlg, DWLP_USER));
        if (msg == WM_INITDIALOG) {
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            self = reinterpret_cast<TextPrompt*>(lp);
            SetDlgItemTextW(dlg, 101, self->value->c_str());
            SetForegroundWindow(dlg);
            return TRUE;
        }
        if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
            if (LOWORD(wp) == IDOK && self) {
                wchar_t buf[256] = {};
                GetDlgItemTextW(dlg, 101, buf, 256);
                *self->value = buf;
            }
            EndDialog(dlg, LOWORD(wp));
            return TRUE;
        }
        return FALSE;
    }
};

// The app icon with a small status badge in the bottom-right corner (grey / amber / green / red).
HICON MakeBadgedIcon(HINSTANCE instance, COLORREF badge) {
    const int size = GetSystemMetrics(SM_CXSMICON);
    HICON app = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
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
    HDC screen = GetDC(nullptr);
    HBITMAP color_bmp = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(mem, color_bmp);
    auto* px = static_cast<uint32_t*>(bits);
    std::fill(px, px + size * size, 0u);
    if (app) DrawIconEx(mem, 0, 0, app, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(mem, old);
    DeleteDC(mem);
    if (app) DestroyIcon(app);

    // Badge: filled circle with a dark outline so it reads on light and dark taskbars.
    const float r = size * 0.30f, c = size - r - 0.5f, ring = std::max(1.0f, size / 16.0f);
    const uint32_t rgb = (GetRValue(badge) << 16) | (GetGValue(badge) << 8) | GetBValue(badge);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float d = std::hypot(x - c, y - c);
            const float outer = std::clamp(r + ring + 0.5f - d, 0.0f, 1.0f);
            if (outer <= 0) continue;
            const float inner = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            // Composite: dark ring under the coloured dot, over the icon pixel.
            uint32_t& p = px[y * size + x];
            auto blend = [&](uint32_t col, float a) {
                const uint32_t dst = p;
                const float da = ((dst >> 24) & 0xFF) / 255.0f;
                const float oa = a + da * (1 - a);
                auto ch = [&](int shift) {
                    const float sc = ((col >> shift) & 0xFF) / 255.0f, dc = ((dst >> shift) & 0xFF) / 255.0f;  // dst premultiplied
                    return uint32_t(std::clamp((sc * a + dc * (1 - a)) * 255.0f + 0.5f, 0.0f, 255.0f));
                };
                p = (uint32_t(oa * 255 + 0.5f) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
            };
            blend(0x1C1C1E, outer);
            blend(rgb, inner);
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
    case shm::AgentStatus::NoDevice: return L"Waiting for iPhone";
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
    icons_[0] = MakeBadgedIcon(instance, RGB(142, 142, 147));  // no device
    icons_[1] = MakeBadgedIcon(instance, RGB(255, 159, 10));   // app not open / paused
    icons_[2] = MakeBadgedIcon(instance, RGB(48, 209, 88));    // connected
    icons_[3] = MakeBadgedIcon(instance, RGB(255, 59, 48));    // live (like the LIVE chip)
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
    if (s.transport == "wifi") tip += L" (Wi-Fi)";
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
    if (s.transport == "wifi") status += L" (Wi-Fi)";
    else if (s.transport == "usb") status += L" (USB)";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_STRING, kPreview, L"Open Tethercam");
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
    HMENU quality = CreatePopupMenu();
    const proto::Camera* selected = nullptr;
    for (const auto& c : s.cameras)
        if (c.camera_id == s.selected_camera) selected = &c;
    for (UINT i = 0; i <= 4; ++i) {
        const auto q = proto::Quality(i);
        const bool supported = !selected || SupportsQuality(selected->formats, q);
        const auto label = i == 0 ? L"Auto (match video app)" : Widen(QualityLabel(q));
        AppendMenuW(quality, MF_STRING | (s.quality == q ? MF_CHECKED : 0) | (supported ? 0 : MF_GRAYED),
                    kQualityBase + i, label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(quality), L"Capture quality");
    AppendMenuW(menu, MF_STRING | (s.mirror ? MF_CHECKED : 0), kMirror, L"Mirror");
    HMENU rotate = CreatePopupMenu();
    const std::pair<UINT, uint16_t> rotations[] = {
        {kRotate0, uint16_t(0)}, {kRotate90, uint16_t(90)}, {kRotate180, uint16_t(180)}, {kRotate270, uint16_t(270)}};
    for (auto [id, deg] : rotations)
        AppendMenuW(rotate, MF_STRING | (s.rotation == deg ? MF_CHECKED : 0), id, std::format(L"{}°", deg).c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rotate), L"Rotate");
    HMENU wireless = CreatePopupMenu();
    if (s.wifi_phone.empty()) {
        AppendMenuW(wireless, MF_STRING | MF_GRAYED, 0, L"Not paired");
        AppendMenuW(wireless, MF_STRING | MF_GRAYED, 0, L"Turn on Wireless in the iPhone app, then connect by USB once");
    } else {
        AppendMenuW(wireless, MF_STRING | MF_GRAYED, 0, (L"Paired: " + Widen(s.wifi_phone)).c_str());
        const std::wstring addr = s.wifi_address.empty() ? L"found automatically" : Widen(s.wifi_address);
        AppendMenuW(wireless, MF_STRING, kWifiAddress, (L"Address: " + addr + L"\u2026").c_str());
        AppendMenuW(wireless, MF_STRING, kWifiForget, L"Forget Wi-Fi pairing");
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(wireless), L"Wireless");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.status_card ? MF_CHECKED : 0), kStatusCard,
                L"Show status card while the camera is offline");
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
    else if (cmd == kStatusCard) engine_.SetStatusCard(!s.status_card);
    else if (cmd == kOpenLogs) ShellExecuteW(nullptr, L"open", log_dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == kMirror) engine_.SetMirror(!s.mirror);
    else if (cmd == kWifiForget) engine_.ForgetWifi();
    else if (cmd == kWifiAddress) {
        std::wstring value = Widen(s.wifi_address);
        if (TextPrompt::Ask(nullptr, L"Tethercam Wi-Fi address",
                            L"iPhone address (host or IP, optional :port). Leave empty to find it automatically.", value))
            engine_.SetWifiAddress(Narrow(value));
    }
    else if (cmd >= kRotate0 && cmd <= kRotate270) engine_.SetRotation(uint16_t((cmd - kRotate0) * 90));
    else if (cmd >= kQualityBase && cmd <= kQualityBase + 4) engine_.SetQuality(proto::Quality(cmd - kQualityBase));
    else if (cmd >= kCameraBase && cmd < kCameraBase + 256) engine_.SelectCamera(int(cmd - kCameraBase));
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
