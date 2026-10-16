# Runs the under-load harness on Windows and prints the M10 numbers: fakephone (the fake iPhone) ->
# tethercam-agent -> vcamcheck read (the Media Foundation source reader, i.e. the Frame Server path
# apps use), first idle and then with CPU burners running. The camera repeats the last frame (or sends
# black) when the agent is late, so the sample rate always looks like 60 fps; frame loss is therefore
# 1 - new frames / source fps, where "new" = the picture changed (vcamcheck's "new frames" line).
#
#   powershell -ExecutionPolicy Bypass -File windows\scripts\load-test.ps1
#   powershell -ExecutionPolicy Bypass -File windows\scripts\load-test.ps1 -Load 24 -Seconds 20
#
# Needs a built tree (build\vs2026-x64) and the camera registered (the MSI, or an elevated
# windows\scripts\dev-install.ps1). The agent is started with --console --tcp 127.0.0.1:5321; any
# already-running tethercam-agent is stopped first and left stopped. Load N = N CPU-bound threads
# in a helper process; load24 on the 16-thread desktop is the M10 target (<= 10 % frame loss).
# -Load 0 measures the idle phase only.
param(
    [int]$Load = 24,
    [string]$Size = "1920x1080",
    [int]$Fps = 60,
    [int]$Seconds = 20,
    [int]$IdleSeconds = 8,
    [string]$Configuration = "Release",
    [switch]$SkipIdle
)
$ErrorActionPreference = "Stop"
# A leftover Local namespace (vcamcheck direct testing) would hide the agent from Frame Server.
Remove-Item Env:TETHERCAM_SHM_NAMESPACE -ErrorAction SilentlyContinue

$repo = Resolve-Path "$PSScriptRoot\..\.."
$build = Join-Path $repo "build\vs2026-x64"
$agentExe = Join-Path $build "agent\$Configuration\tethercam-agent.exe"
$fakephoneExe = Join-Path $build "tools\fakephone\$Configuration\fakephone.exe"
$vcamcheckExe = Join-Path $build "tools\vcamcheck\$Configuration\vcamcheck.exe"

$outDir = Join-Path $env:TEMP "tethercam-loadtest"
New-Item -ItemType Directory -Force $outDir | Out-Null
$agentLog = Join-Path $outDir "agent.out"
$agentErr = Join-Path $outDir "agent.err"
$fakeLog = Join-Path $outDir "fakephone.out"
$fakeErr = Join-Path $outDir "fakephone.err"

foreach ($tool in @($agentExe, $fakephoneExe, $vcamcheckExe)) {
    if (-not (Test-Path $tool)) {
        throw "missing $tool`nbuild first (in windows\):  cmake --preset vs2026-x64 ; cmake --build --preset $($Configuration.ToLower())"
    }
}

function Get-Delivered($file) {
    # "read 1200 frames at 1920x1080 in 20.02 s: 59.9 fps (sample clock 60.0 fps)"
    # "new frames 1150: 57.6 fps, gaps > 50 ms 1.2 % (14 of 1149), longest gap 83 ms"
    $text = Get-Content $file -Raw
    $m = [regex]::Match($text, '([0-9.]+) fps \(sample clock ([0-9.]+) fps\)')
    $n = [regex]::Match($text, 'new frames \d+: ([0-9.]+) fps, gaps > 50 ms ([0-9.]+) %.*longest gap ([0-9]+) ms')
    if (-not $m.Success -or -not $n.Success) { throw "cannot parse vcamcheck output in $file`n$text" }
    return [pscustomobject]@{
        Samples = [double]$m.Groups[1].Value; Clock = [double]$m.Groups[2].Value
        New = [double]$n.Groups[1].Value; Gaps = [double]$n.Groups[2].Value; Longest = [int]$n.Groups[3].Value
    }
}

function Stop-Harness {
    Get-Process tethercam-agent, fakephone -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

$probe = & $vcamcheckExe list 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { throw "vcamcheck list failed:`n$probe" }
if ($probe -notmatch "Tethercam") {
    throw "the Tethercam camera is not registered - install the MSI, or run the elevated windows\scripts\dev-install.ps1"
}

$burner = $null
try {
    Stop-Harness
    Start-Sleep -Seconds 1
    Write-Host "fakephone -> agent -> vcamcheck read, $Size @ $Fps, load $Load ..."

    Start-Process -FilePath $fakephoneExe -ArgumentList "--port", "5321" -WindowStyle Hidden `
        -RedirectStandardOutput $fakeLog -RedirectStandardError $fakeErr | Out-Null
    Start-Process -FilePath $agentExe -ArgumentList "--console", "--tcp", "127.0.0.1:5321" -WindowStyle Hidden `
        -RedirectStandardOutput $agentLog -RedirectStandardError $agentErr | Out-Null

    # The phone only streams once a consumer opens the camera, so wait for the connection (the agent
    # logs the phone's cameras) and let the first vcamcheck read start the stream.
    $connected = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $agentErr) -and (Select-String -Path $agentErr -Pattern "camera \d+ '" -Quiet)) { $connected = $true; break }
    }
    if (-not $connected) {
        Write-Host "--- agent output ---`n$(Get-Content $agentErr -Tail 15 -ErrorAction SilentlyContinue)"
        throw "the agent never connected to fakephone"
    }

    $idle = $null
    if (-not $SkipIdle) {
        & $vcamcheckExe read --size $Size --fps $Fps --frames ($IdleSeconds * $Fps) | Tee-Object -FilePath (Join-Path $outDir "vcamcheck-idle.txt")
        if ($LASTEXITCODE -ne 0) { throw "idle vcamcheck run failed" }
        $idle = Get-Delivered (Join-Path $outDir "vcamcheck-idle.txt")
    }

    if ($Load -gt 0) {
        $burnPs1 = Join-Path $outDir "burn.ps1"
        Set-Content -Encoding ASCII $burnPs1 @"
param([int]`$Threads)
`$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @'
using System.Threading;
public static class TcBurn {
    public static void Start(int n) {
        for (int i = 0; i < n; i++) {
            var t = new Thread(() => { while (true) { Thread.SpinWait(1000000); } });
            t.IsBackground = true; t.Start();
        }
    }
}
'@
[TcBurn]::Start(`$Threads)
Start-Sleep -Seconds 3600
"@
        $burner = Start-Process powershell.exe -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $burnPs1, "-Threads", $Load -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $outDir "burner.out") -RedirectStandardError (Join-Path $outDir "burner.err")
        Write-Host "burners up ($Load threads), settling 3 s ..."
        Start-Sleep -Seconds 3
        if ($burner.HasExited) { throw "the CPU burners exited early; see $outDir\burner.err" }
    }

    $under = $null
    if ($Load -gt 0) {
        & $vcamcheckExe read --size $Size --fps $Fps --frames ($Seconds * $Fps) | Tee-Object -FilePath (Join-Path $outDir "vcamcheck-load.txt")
        if ($LASTEXITCODE -ne 0) { throw "load vcamcheck run failed" }
        $under = Get-Delivered (Join-Path $outDir "vcamcheck-load.txt")
    }
} finally {
    if ($burner) { Stop-Process -Id $burner.Id -Force -ErrorAction SilentlyContinue }
    Stop-Harness
}

function Format-Run($label, $r) {
    "{0,-7}: {1,5:N1} new fps, frame loss {2,5:N1} %, gaps > 50 ms {3,5:N1} %, longest {4} ms, sample clock {5:N1} fps ({6:N1} samples/s)" -f `
        $label, $r.New, ((1 - $r.New / $Fps) * 100), $r.Gaps, $r.Longest, $r.Clock, $r.Samples
}
Write-Host ""
Write-Host "=== Tethercam load test ($Size @ $Fps, load $Load) ==="
if ($idle) { Write-Host (Format-Run "idle" $idle) }
if ($under) { Write-Host (Format-Run "load$Load" $under) }
Write-Host ""
Write-Host "--- agent evidence ---"
Get-Content $agentErr -ErrorAction SilentlyContinue | Select-String -Pattern "media thread:|GPU scale ready|video: " | Select-Object -First 5 | ForEach-Object { $_.Line }
Write-Host ""
Write-Host "logs: $outDir   (record the table in docs/perf.md)"

$fail = @()
foreach ($r in @($idle, $under) | Where-Object { $_ }) {
    if ([math]::Abs($r.Clock - $Fps) -gt 0.5) { $fail += "sample clock {0:N1} fps is not ~{1}" -f $r.Clock, $Fps }
}
if ($idle -and $idle.New -lt $Fps * 0.97) { $fail += "idle delivers {0:N1} new fps (< 97 % of {1})" -f $idle.New, $Fps }
if ($under -and (1 - $under.New / $Fps) -gt 0.10) { $fail += "frame loss at load$Load is above 10 %" }
if ($fail) {
    $fail | ForEach-Object { Write-Host "MISS: $_" }
    exit 1
}
Write-Host "OK: within the M10 target (sample clock ~$Fps, <= 10 % frame loss at load$Load)"
exit 0
