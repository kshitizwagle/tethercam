# Registers a development build of the Tethercam virtual camera machine-wide.
# Run from an elevated PowerShell (the camera's COM server must be registered under HKLM so the
# Frame Server service can load it):
#
#   powershell -ExecutionPolicy Bypass -File windows\scripts\dev-install.ps1 [-Configuration Debug] [-Uninstall]
#
# The binaries are copied to C:\ProgramData\TethercamDev because the FrameServer service runs as
# LocalService and cannot read files inside your user profile.
param(
    [string]$Configuration = "Debug",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated (Administrator) PowerShell."
}
$repo = Resolve-Path "$PSScriptRoot\..\.."
$build = Join-Path $repo "build\vs2026-x64"
$dest = "C:\ProgramData\TethercamDev"

if ($Uninstall) {
    if (Test-Path "$dest\tethercam-agent.exe") { & "$dest\tethercam-agent.exe" --uninstall | Out-Host }
    Get-Process tethercam-agent -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Removed the development camera."
    return
}

New-Item -ItemType Directory -Force $dest | Out-Null
Get-Process tethercam-agent -ErrorAction SilentlyContinue | Stop-Process -Force
Copy-Item (Join-Path $build "agent\$Configuration\tethercam-agent.exe") $dest -Force
Copy-Item (Join-Path $build "vcam\$Configuration\tethercam-vcam.dll") $dest -Force
$pdb = Join-Path $build "vcam\$Configuration\tethercam-vcam.pdb"
if (Test-Path $pdb) { Copy-Item $pdb $dest -Force }
& "$dest\tethercam-agent.exe" --install | Out-Host
if ($LASTEXITCODE -ne 0) { throw "registration failed" }
Write-Host "Installed. Start the agent (not elevated):  $dest\tethercam-agent.exe --console"
Write-Host "Check the camera:                           build\vs2026-x64\tools\vcamcheck\$Configuration\vcamcheck.exe list"
