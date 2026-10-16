# Windows load-test runbook (M10)

One-shot checklist for the dual-boot PC, from the repo root. CMake is not on the plain PowerShell PATH;
use a Developer PowerShell for VS 2026, or prepend
`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.

```
cd windows
cmake --preset vs2026-x64
cmake --build --preset release
cd ..
# elevated PowerShell, once per build: registers this build's camera DLL (the MSI may be older)
powershell -ExecutionPolicy Bypass -File windows\scripts\dev-install.ps1 -Configuration Release
# normal PowerShell:
powershell -ExecutionPolicy Bypass -File windows\scripts\load-test.ps1
```

The harness pairs the dev-tree agent with whichever camera DLL is registered, so register the same build
(`vcamcheck list` must show `Tethercam`). To go back to the installed app afterwards:
`dev-install.ps1 -Uninstall` (elevated), then repair or reinstall the MSI.

What it does: stops any running tethercam-agent (including the installed tray app; it is left stopped),
starts fakephone + the agent (`--console --tcp 127.0.0.1:5321`), waits until the agent has connected
(the phone only streams once a consumer opens the camera), then reads through Frame Server
(`vcamcheck read`) idle (8 s) and under load24 (24 CPU-burner threads, 20 s). Logs:
`%TEMP%\tethercam-loadtest` (the agent's log is `agent.err`); knobs: `-Load`, `-Size`, `-Fps`,
`-Seconds`, `-IdleSeconds`, `-Configuration`, `-SkipIdle` (`-Load 0` = idle only).

Metrics: the camera repeats the last frame (or sends black) whenever the agent is late, so its sample
rate reads ~60 fps no matter what. The harness scores **new frames** (the picture changed; the first
second of video is skipped as start-up), **gaps > 50 ms** between new frames (comparable with the Linux
table in `docs/perf.md`), the longest gap, and the **sample clock**.

Pass (the script checks all three): sample clock ≈ 60.0 in both runs, idle ≥ 97 % new frames (58.2 fps),
frame loss (1 − new fps / 60) ≤ 10 % at load24. Then, same Windows session: the Streamlabs latency spot
check (real iPhone, clock screenshots). Record both in `docs/perf.md` and tick the M10 items in
`TODO.md` / `MILESTONES.md`.
