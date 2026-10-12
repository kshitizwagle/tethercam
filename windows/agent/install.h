#pragma once

#include <memory>
#include <string>

struct IMFVirtualCamera;

namespace tethercam::agent {

// Registers the COM server in tethercam-vcam.dll and the Windows 11 virtual camera.
// Machine-wide (admin) by default; per_user registers for the current user only.
bool InstallCamera(const std::wstring& dll_path, bool per_user, std::wstring& error);
bool UninstallCamera(const std::wstring& dll_path, bool per_user, std::wstring& error);

// Creates a camera that exists only while the returned object lives (development without admin).
class SessionCamera {
public:
    ~SessionCamera();
    bool Create(const std::wstring& dll_path, std::wstring& error);

private:
    IMFVirtualCamera* camera_ = nullptr;
};

// Path of tethercam-vcam.dll next to the running executable.
std::wstring DefaultDllPath();

}  // namespace tethercam::agent
