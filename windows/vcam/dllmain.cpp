// tethercam-vcam: Frame Server media source loaded by MFCreateVirtualCamera.
// Runs inside the FrameServer service as LocalService (see docs/TECH_SPEC.md §8).

#include <string>

#include "vcam.h"

using namespace Microsoft::WRL;
using tethercam::vcam::Activator;

CoCreatableClass(Activator);

namespace {

HMODULE g_module = nullptr;

// Registers the COM server under HKLM (machine-wide, needed by the FrameServer service)
// or under HKCU when `per_user` is set.
HRESULT WriteRegistration(bool per_user) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + tethercam::kVcamClsidString;
    HKEY root = per_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    HKEY clsid = nullptr, inproc = nullptr;
    LSTATUS s = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &clsid, nullptr);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    RegSetValueExW(clsid, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(L"Tethercam Virtual Camera"),
                   sizeof(L"Tethercam Virtual Camera"));
    s = RegCreateKeyExW(clsid, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
    if (s == ERROR_SUCCESS) {
        RegSetValueExW(inproc, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path),
                       DWORD((wcslen(path) + 1) * sizeof(wchar_t)));
        RegSetValueExW(inproc, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"Both"),
                       sizeof(L"Both"));
        RegCloseKey(inproc);
    }
    RegCloseKey(clsid);
    return HRESULT_FROM_WIN32(s);
}

HRESULT DeleteRegistration(bool per_user) {
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + tethercam::kVcamClsidString;
    const LSTATUS s = RegDeleteTreeW(per_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, key.c_str());
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    return Module<InProc>::GetModule().GetClassObject(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow() { return Module<InProc>::GetModule().Terminate() ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() { return WriteRegistration(false); }
STDAPI DllUnregisterServer() { return DeleteRegistration(false); }

// Per-user variants used by `tethercam-agent --install --user` (no admin rights).
STDAPI TethercamRegisterUser() { return WriteRegistration(true); }
STDAPI TethercamUnregisterUser() { return DeleteRegistration(true); }
