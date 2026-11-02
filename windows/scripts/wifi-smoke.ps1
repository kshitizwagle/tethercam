# End-to-end Wi-Fi check without an iPhone (M11), run by CI on every Windows build:
#   1. fakephone + agent --tcp: the plain port stands in for USB, so the agent pairs (WifiPair/WifiPaired)
#      and writes the key to a pairing file.
#   2. A fresh agent with the same pairing file and no --tcp: usbmuxd is absent, so it falls back to
#      Wi-Fi (Bonjour advert, then the address fakephone reported), opens the encrypted session, and the
#      status API must report "transport":"wifi".
#   3. vcamcheck direct loads the camera in-process (Local shm namespace, no registration or admin) and
#      reads frames: they must be new frames from the phone, not black.
#
#   powershell -ExecutionPolicy Bypass -File windows\scripts\wifi-smoke.ps1 [-Build build\vs2026-x64] [-Configuration Release]
#
# Stops any running tethercam-agent / fakephone first and leaves them stopped.
param(
    [string]$Build = "build\vs2026-x64",
    [string]$Configuration = "Release",
    [string]$Size = "1920x1080",
    [int]$Fps = 30,
    [int]$Seconds = 6
)
$ErrorActionPreference = "Stop"

$repo = Resolve-Path "$PSScriptRoot\..\.."
$buildDir = if ([IO.Path]::IsPathRooted($Build)) { $Build } else { Join-Path $repo $Build }
$agentExe = Join-Path $buildDir "agent\$Configuration\tethercam-agent.exe"
$fakephoneExe = Join-Path $buildDir "tools\fakephone\$Configuration\fakephone.exe"
$vcamcheckExe = Join-Path $buildDir "tools\vcamcheck\$Configuration\vcamcheck.exe"
$vcamDll = Get-ChildItem -Path $buildDir -Recurse -Filter "tethercam-vcam.dll" |
    Where-Object { $_.FullName -match "\\$Configuration\\" } | Select-Object -First 1 -ExpandProperty FullName
foreach ($tool in @($agentExe, $fakephoneExe, $vcamcheckExe, $vcamDll)) {
    if (-not $tool -or -not (Test-Path $tool)) { throw "missing build output ($tool); build $Configuration first" }
}

$outDir = Join-Path $env:TEMP "tethercam-wifi-smoke"
Remove-Item -Recurse -Force $outDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $outDir | Out-Null
$pairingFile = Join-Path $outDir "pairing.dat"
# The agent and the in-process camera meet in the session namespace (no registered camera needed).
$env:TETHERCAM_SHM_NAMESPACE = "Local"

function Stop-Harness {
    Get-Process tethercam-agent, fakephone -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Start-Tool($exe, $argList, $name) {
    Start-Process -FilePath $exe -ArgumentList $argList -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $outDir "$name.out") -RedirectStandardError (Join-Path $outDir "$name.err")
}

function Wait-Log($name, $pattern, $seconds) {
    $file = Join-Path $outDir "$name.err"
    for ($i = 0; $i -lt $seconds * 4; $i++) {
        if ((Test-Path $file) -and (Select-String -Path $file -Pattern $pattern -Quiet)) { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Get-Status {
    try { return Invoke-RestMethod -Uri "http://127.0.0.1:4790/status" -TimeoutSec 2 } catch { return $null }
}

function Show-Logs {
    foreach ($f in Get-ChildItem $outDir -Filter "*.err") {
        Write-Host "--- $($f.Name) ---"
        Get-Content $f.FullName -Tail 25 -ErrorAction SilentlyContinue
    }
}

$fail = @()
try {
    Stop-Harness
    Start-Sleep -Seconds 1
    $null = Start-Tool $fakephoneExe @("--port", "5321", "--wifi-port", "5322", "--advertise") "fakephone"

    # 1. Pair over the stand-in USB link.
    $pairAgent = Start-Tool $agentExe @("--console", "--tcp", "127.0.0.1:5321", "--pairing-file", $pairingFile) "agent-pair"
    if (-not (Wait-Log "agent-pair" "paired with .* for Wi-Fi" 20)) { Show-Logs; throw "the agent did not pair over --tcp" }
    if (-not (Test-Path $pairingFile)) { Show-Logs; throw "pairing reported but $pairingFile was not written" }
    Write-Host "paired over the stand-in USB link"
    Stop-Process -Id $pairAgent.Id -Force
    Start-Sleep -Seconds 1

    # 2. Fresh agent, same pairing, no USB: must come up over Wi-Fi.
    $null = Start-Tool $agentExe @("--console", "--pairing-file", $pairingFile) "agent-wifi"
    $status = $null
    for ($i = 0; $i -lt 60; $i++) {
        $status = Get-Status
        if ($status -and $status.transport -eq "wifi") { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $status -or $status.transport -ne "wifi") {
        Show-Logs; throw "the agent never reported transport=wifi (last status: $($status | ConvertTo-Json -Compress))"
    }
    $how = if (Select-String -Path (Join-Path $outDir "agent-wifi.err") -Pattern "Bonjour: " -Quiet) { "found by Bonjour" } else { "reported address (no Bonjour result)" }
    Write-Host "connected over Wi-Fi ($how): $($status | ConvertTo-Json -Compress)"

    # 3. Read frames through the camera; opening it is what makes the phone stream.
    # The status must say live over Wi-Fi while frames flow (the phone stops 2 s after the camera closes).
    $reader = Start-Tool $vcamcheckExe @("direct", "--dll", $vcamDll, "--size", $Size, "--fps", $Fps, "--frames", ($Seconds * $Fps)) "vcamcheck"
    $null = $reader.Handle  # keeps ExitCode readable after exit
    $sawLive = $false
    while (-not $reader.HasExited) {
        $s = Get-Status
        if ($s -and $s.live -and $s.transport -eq "wifi") { $sawLive = $true }
        Start-Sleep -Milliseconds 300
    }
    $reader.WaitForExit()
    $text = (Get-Content (Join-Path $outDir "vcamcheck.out") -Raw) + (Get-Content (Join-Path $outDir "vcamcheck.err") -Raw)
    Write-Host $text
    if ($reader.ExitCode -ne 0) { $fail += "vcamcheck direct exited with $($reader.ExitCode)" }
    if (-not $sawLive) { $fail += "the status API never reported live over Wi-Fi while the camera was read" }
    $n = [regex]::Match($text, 'new frames \d+: ([0-9.]+) fps, gaps > 50 ms ([0-9.]+) %.*longest gap ([0-9]+) ms')
    if (-not $n.Success) {
        $fail += "cannot parse vcamcheck output"
    } else {
        $newFps = [double]$n.Groups[1].Value
        Write-Host ("over Wi-Fi: {0:N1} new fps of {1}, gaps > 50 ms {2} %, longest {3} ms" -f $newFps, $Fps, $n.Groups[2].Value, $n.Groups[3].Value)
        # Shared CI runners are slow and noisy: this checks the path works, not its performance.
        if ($newFps -lt $Fps * 0.5) { $fail += "only $newFps new fps over Wi-Fi (< half of $Fps)" }
    }
} finally {
    Stop-Harness
}

if ($fail) {
    Show-Logs
    $fail | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
Write-Host "OK: paired over the stand-in USB link, streamed over encrypted Wi-Fi through the camera"
exit 0
