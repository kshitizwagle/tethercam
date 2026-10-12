#include "install.h"

#include <windows.h>

#include <mfapi.h>
#include <mfvirtualcamera.h>

#include <format>

#include "tethercam/log.h"
#include "tethercam/vcam_ids.h"

namespace tethercam::agent {
namespace {

std::wstring HrText(HRESULT hr) { return std::format(L"0x{:08X}", static_cast<uint32_t>(hr)); }

bool IsUnderUserProfile(const std::wstring& path) {
    wchar_t profile[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return n && n < MAX_PATH && _wcsnicmp(path.c_str(), profile, n) == 0;
}

// Calls an exported registration function in the vcam DLL.
HRESULT CallDllExport(const std::wstring& dll_path, const char* name) {
    HMODULE m = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    using Fn = HRESULT(STDAPICALLTYPE*)();
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, name));
    const HRESULT hr = fn ? fn() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    FreeLibrary(m);
    return hr;
}

HRESULT CreateCamera(MFVirtualCameraLifetime lifetime, MFVirtualCameraAccess access, IMFVirtualCamera** out) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, lifetime, access, kVcamFriendlyName,
                                 kVcamClsidString, nullptr, 0, out);
}

}  // namespace

std::wstring DefaultDllPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L"\\/") + 1);
    return path + L"tethercam-vcam.dll";
}

bool InstallCamera(const std::wstring& dll_path, bool per_user, std::wstring& error) {
    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"cannot find " + dll_path;
        return false;
    }
    if (IsUnderUserProfile(dll_path))
        log::Warn("the camera DLL is inside the user profile; the Frame Server service (LocalService) "
                  "may not be able to load it. Install to Program Files or ProgramData.");
    HRESULT hr = CallDllExport(dll_path, per_user ? "TethercamRegisterUser" : "DllRegisterServer");
    if (FAILED(hr)) {
        error = L"COM registration failed " + HrText(hr) + (per_user ? L"" : L" (run as administrator)");
        return false;
    }
    IMFVirtualCamera* cam = nullptr;
    hr = CreateCamera(MFVirtualCameraLifetime_System,
                      per_user ? MFVirtualCameraAccess_CurrentUser : MFVirtualCameraAccess_AllUsers, &cam);
    if (FAILED(hr)) {
        error = L"MFCreateVirtualCamera failed " + HrText(hr);
        return false;
    }
    hr = cam->Start(nullptr);
    cam->Shutdown();
    cam->Release();
    if (FAILED(hr)) {
        error = L"starting the virtual camera failed " + HrText(hr);
        return false;
    }
    return true;
}

bool UninstallCamera(const std::wstring& dll_path, bool per_user, std::wstring& error) {
    IMFVirtualCamera* cam = nullptr;
    HRESULT hr = CreateCamera(MFVirtualCameraLifetime_System,
                              per_user ? MFVirtualCameraAccess_CurrentUser : MFVirtualCameraAccess_AllUsers, &cam);
    if (SUCCEEDED(hr)) {
        hr = cam->Remove();
        cam->Shutdown();
        cam->Release();
    }
    if (FAILED(hr)) error = L"removing the virtual camera failed " + HrText(hr);
    const HRESULT reg = CallDllExport(dll_path, per_user ? "TethercamUnregisterUser" : "DllUnregisterServer");
    if (FAILED(reg) && error.empty()) error = L"COM unregistration failed " + HrText(reg);
    return SUCCEEDED(hr) && SUCCEEDED(reg);
}

SessionCamera::~SessionCamera() {
    if (camera_) {
        camera_->Remove();
        camera_->Shutdown();
        camera_->Release();
    }
}

bool SessionCamera::Create(const std::wstring& dll_path, std::wstring& error) {
    HRESULT hr = CallDllExport(dll_path, "TethercamRegisterUser");
    if (FAILED(hr)) {
        error = L"COM registration failed " + HrText(hr);
        return false;
    }
    hr = CreateCamera(MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser, &camera_);
    if (FAILED(hr)) {
        error = L"MFCreateVirtualCamera failed " + HrText(hr);
        return false;
    }
    hr = camera_->Start(nullptr);
    if (FAILED(hr)) {
        error = L"starting the session camera failed " + HrText(hr);
        return false;
    }
    return true;
}

}  // namespace tethercam::agent
