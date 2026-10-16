# Windows load-test runbook (M10)

One-shot checklist for the dual-boot PC. From a normal (non-elevated) PowerShell in the repo root:

```
git pull                                     # main must be 47e3bff or newer (the harness + this runbook)
cd windows
cmake --preset vs2026-x64
cmake --build --preset release               # Debug also fine, pass -Configuration Debug
powershell -ExecutionPolicy Bypass -File scripts\load-test.ps1
```

Prerequisites: the camera must be registered — the v0.1.12 MSI, or an elevated
`windows\scripts\dev-install.ps1` for the dev tree; the harness tests whichever is registered.

What it does: stops any running tethercam-agent, starts fakephone + the agent
(`--console --tcp 127.0.0.1:5321`), reads through Frame Server (`vcamcheck read`) idle (8 s) and then
under load24 (24 CPU-burner threads, 20 s), and prints delivered fps, sample clock and frame loss plus
the agent's `media thread:` and `GPU scale ready:` lines. Logs: `%TEMP%\tethercam-loadtest`; knobs:
`-Load`, `-Size`, `-Fps`, `-Seconds`, `-Configuration`, `-SkipIdle` (`-Load 0` = idle only).

Pass: sample clock ≈ 60.0 in both runs, idle ≈ 60 fps, frame loss ≤ 10 % at load24. Then, same Windows
session: the Streamlabs latency spot check (real iPhone, clock screenshots). Record both in
`docs/perf.md` and tick the M10 items in `TODO.md` / `MILESTONES.md`.
