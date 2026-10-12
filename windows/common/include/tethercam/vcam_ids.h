#pragma once

// Identity of the Tethercam virtual camera, shared by the DLL and the agent's installer.

#include <guiddef.h>

namespace tethercam {

// CLSID of the media source activator in tethercam-vcam.dll.
inline constexpr wchar_t kVcamClsidString[] = L"{9C8F9375-C87A-46A7-8BFD-552ED47E75BA}";
inline constexpr GUID kVcamClsid = {0x9c8f9375, 0xc87a, 0x46a7, {0x8b, 0xfd, 0x55, 0x2e, 0xd4, 0x7e, 0x75, 0xba}};
inline constexpr wchar_t kVcamFriendlyName[] = L"Tethercam";

}  // namespace tethercam
