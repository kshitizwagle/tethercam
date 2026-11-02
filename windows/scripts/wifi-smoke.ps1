# End-to-end Wi-Fi check without an iPhone (M11), run by CI on every Windows build:
#   1. fakephone + agent --tcp: the plain port stands in for USB, so the agent pairs (WifiPair/WifiPaired)
#      and writes the key to a pairing file; frames read over this link are the machine's baseline.
#   2. A fresh agent with the same pairing file and no --tcp: usbmuxd is absent, so it falls back to
#      Wi-Fi (Bonjour advert, then the address fakephone reported), opens the encrypted session, and the
#      status API must report "transport":"wifi".
#   3. vcamcheck direct loads the camera in-process (Local shm namespace, no registration or admin) and
#      reads frames: they must be new frames from the phone, at least half the baseline rate.
#
#   powershell -ExecutionPolicy Bypass -File windows\scripts\wifi-smoke.ps1 [-Build build\vs2026-x64] [-Configuration Release]
#
# Stops any running tethercam-agent / fakephone first and leaves them stopped.
param(
    [string]$Build = "build\vs2026-x64",
    [string]$Configuration = "Release",
    [string]$Size = "1920x1080",
    [int]$Fps = 30,
    [int]$Seconds = 10
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

$script:fail = @()
# Opens the camera in-process for $Seconds (opening it is what makes the phone stream) and returns the
# new-frame rate. The status API must report live over $transport while frames flow (the phone stops
# 2 s after the camera closes, so it is checked during the read, not after).
function Read-Camera($transport) {
    $name = "vcamcheck-$transport"
    $reader = Start-Tool $vcamcheckExe @("direct", "--dll", $vcamDll, "--size", $Size, "--fps", $Fps, "--frames", ($Seconds * $Fps)) $name
    $null = $reader.Handle  # keeps ExitCode readable after exit
    $sawLive = $false
    while (-not $reader.HasExited) {
        $s = Get-Status
        if ($s -and $s.live -and $s.transport -eq $transport) { $sawLive = $true }
        Start-Sleep -Milliseconds 300
    }
    $reader.WaitForExit()
    $text = (Get-Content (Join-Path $outDir "$name.out") -Raw) + (Get-Content (Join-Path $outDir "$name.err") -Raw)
    Write-Host $text
    if ($reader.ExitCode -ne 0) { $script:fail += "vcamcheck over $transport exited with $($reader.ExitCode)" }
    if (-not $sawLive) { $script:fail += "the status API never reported live over $transport while the camera was read" }
    $n = [regex]::Match($text, 'new frames \d+: ([0-9.]+) fps, gaps > 50 ms ([0-9.]+) %.*longest gap ([0-9]+) ms')
    if (-not $n.Success) { $script:fail += "cannot parse vcamcheck output over $transport"; return 0.0 }
    Write-Host ("over {0}: {1} new fps of {2}, gaps > 50 ms {3} %, longest {4} ms" -f $transport, $n.Groups[1].Value, $Fps, $n.Groups[2].Value, $n.Groups[3].Value)
    return [double]$n.Groups[1].Value
}

try {
    Stop-Harness
    Start-Sleep -Seconds 1
    $null = Start-Tool $fakephoneExe @("--port", "5321", "--wifi-port", "5322", "--advertise") "fakephone"

    # 1. Pair over the stand-in USB link, and stream over it once as the baseline for this machine
    #    (CI runners have no GPU: fakephone encodes and the agent decodes in software).
    $usbAgent = Start-Tool $agentExe @("--console", "--tcp", "127.0.0.1:5321", "--pairing-file", $pairingFile) "agent-usb"
    if (-not (Wait-Log "agent-usb" "paired with .* for Wi-Fi" 20)) { Show-Logs; throw "the agent did not pair over --tcp" }
    if (-not (Test-Path $pairingFile)) { Show-Logs; throw "pairing reported but $pairingFile was not written" }
    Write-Host "paired over the stand-in USB link"
    $usbFps = Read-Camera "usb"
    Stop-Process -Id $usbAgent.Id -Force
    Start-Sleep -Seconds 3  # let fakephone stop before the next host connects

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
    Write-Host "connected over Wi-Fi ($how)"

    # 3. Frames through the camera over Wi-Fi. Shared runners are slow and noisy, so this checks that
    #    the encrypted path works and keeps up with the plain link, not absolute performance.
    $wifiFps = Read-Camera "wifi"
    Write-Host ("new fps: usb {0:N1}, wifi {1:N1}" -f $usbFps, $wifiFps)
    if ($wifiFps -lt 1) { $fail += "no video over Wi-Fi ($wifiFps new fps)" }
    elseif ($wifiFps -lt $usbFps * 0.5) { $fail += "Wi-Fi delivers $wifiFps new fps, under half the plain link's $usbFps" }
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
