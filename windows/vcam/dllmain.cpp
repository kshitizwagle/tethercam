// tethercam-vcam: Frame Server media source loaded by MFCreateVirtualCamera.
// Runs inside the FrameServer service as LocalService (see docs/TECH_SPEC.md §8).
// Skeleton only (M0): no class is registered yet.
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* ppv) {
    if (ppv) *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return S_OK; }
STDAPI DllRegisterServer() { return E_NOTIMPL; }
STDAPI DllUnregisterServer() { return E_NOTIMPL; }
