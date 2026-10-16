# Runs the under-load harness on Windows and prints the M10 numbers: fakephone (the fake iPhone) ->
# tethercam-agent -> vcamcheck read (the Media Foundation source reader, i.e. the Frame Server path
# apps use), first idle and then with CPU burners running. Frame loss = 1 - delivered / source fps.
#
#   powershell -ExecutionPolicy Bypass -File windows\scripts\load-test.ps1
#   powershell -ExecutionPolicy Bypass -File windows\scripts\load-test.ps1 -Load 24 -Seconds 20
#
# Needs a built tree (build\vs2026-x64) and the camera registered (the MSI, or an elevated
# windows\scripts\dev-install.ps1). The agent is started with --console --tcp 127.0.0.1:5321; any
# already-running tethercam-agent is stopped first and left stopped. Load N = N CPU-bound threads
# in a helper process; load24 on the 16-thread desktop is the M10 target (<= 10 % frame loss).
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
    $text = Get-Content $file -Raw
    $m = [regex]::Match($text, '([0-9.]+) fps \(sample clock ([0-9.]+) fps\)')
    if (-not $m.Success) { throw "cannot parse vcamcheck output in $file`n$text" }
    return @([double]$m.Groups[1].Value, [double]$m.Groups[2].Value)
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

    $streaming = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $agentLog) -and (Select-String -Path $agentLog -Pattern "video: " -Quiet)) { $streaming = $true; break }
    }
    if (-not $streaming) {
        Write-Host "--- agent output ---`n$(Get-Content $agentLog -Tail 15 -ErrorAction SilentlyContinue)"
        throw "the agent never started streaming"
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

    & $vcamcheckExe read --size $Size --fps $Fps --frames ($Seconds * $Fps) | Tee-Object -FilePath (Join-Path $outDir "vcamcheck-load.txt")
    if ($LASTEXITCODE -ne 0) { throw "load vcamcheck run failed" }
    $under = Get-Delivered (Join-Path $outDir "vcamcheck-load.txt")
} finally {
    if ($burner) { Stop-Process -Id $burner.Id -Force -ErrorAction SilentlyContinue }
    Stop-Harness
}

$loss = (1 - $under[0] / $Fps) * 100
Write-Host ""
Write-Host "=== Tethercam load test ($Size @ $Fps, load $Load, ${Seconds}s per probe) ==="
if ($idle) { Write-Host ("idle : {0,5:N1} fps delivered, sample clock {1,5:N1} fps" -f $idle[0], $idle[1]) }
Write-Host ("load{0}: {1,5:N1} fps delivered, sample clock {2,5:N1} fps, frame loss {3:N1} %" -f $Load, $under[0], $under[1], $loss)
Write-Host ""
Write-Host "--- agent evidence ---"
Get-Content $agentLog -ErrorAction SilentlyContinue | Select-String -Pattern "media thread:|GPU scale ready|video: " | Select-Object -First 5 | ForEach-Object { $_.Line }
Write-Host ""
Write-Host "logs: $outDir   (record the table in docs/perf.md)"

if ($Load -gt 0 -and $loss -gt 10) {
    Write-Host "MISS: target is <= 10 % frame loss at load$Load"
    exit 1
}
Write-Host "OK: within the M10 target (<= 10 % frame loss at load$Load)"
exit 0
