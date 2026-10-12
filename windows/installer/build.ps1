# Builds the Tethercam MSI from a Release build.
#
#   cd windows
#   cmake --preset vs2026-x64; cmake --build --preset release
#   .\installer\build.ps1                     # uses `wix` from PATH (dotnet tool install --global wix --version 5.0.2)
#   .\installer\build.ps1 -Wix "dotnet C:\path\to\wix.dll"
param(
    [string]$Wix = "wix",
    [string]$Configuration = "Release",
    [string]$Version = "0.1.0"
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$build = Join-Path $repo "build\vs2026-x64"
$stage = Join-Path $build "installer-stage"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $build "agent\$Configuration\tethercam-agent.exe") $stage -Force
Copy-Item (Join-Path $build "vcam\$Configuration\tethercam-vcam.dll") $stage -Force

$out = Join-Path $build "Tethercam-$Version-x64.msi"
$wixParts = $Wix -split " ", 2
$cmdArgs = @()
if ($wixParts.Count -gt 1) { $cmdArgs += $wixParts[1] }
$cmdArgs += @("build", (Join-Path $PSScriptRoot "Tethercam.wxs"), "-arch", "x64",
    "-d", "Version=$Version", "-d", "BinDir=$stage", "-d", "RepoDir=$repo", "-o", $out)
& $wixParts[0] @cmdArgs
if ($LASTEXITCODE -ne 0) { throw "wix build failed ($LASTEXITCODE)" }
Write-Host "Built $out"
