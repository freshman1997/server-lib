# RTSP Script Index

This folder contains RTSP/RTCP validation scripts for both Windows (PowerShell) and Linux/macOS (Bash).

## Quick Start

- Windows:

```powershell
pwsh -File release/rtsp/scripts/rtsp_preflight.ps1
```

- Linux/macOS:

```bash
bash release/rtsp/scripts/rtsp_preflight.sh
```

## Scripts

### 1) Gate (blocking regression)

- Windows: `run_rtsp_gate.ps1`
- Linux/macOS: `run_rtsp_gate.sh`

Run only gate tests (`rtsp|rtcp`) and fail fast if any test fails.

Examples:

```powershell
pwsh -File release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex "rtsp|rtcp"
```

```bash
bash release/rtsp/scripts/run_rtsp_gate.sh build "rtsp|rtcp"
```

### 2) Soak (iterative long-run)

- Windows: `run_rtsp_soak.ps1`
- Linux/macOS: `run_rtsp_soak.sh`

Loop test execution for a configured duration and output artifacts:

- `driver-<timestamp>.log`
- `iterations-<timestamp>.jsonl`
- `summary-<timestamp>.json`

Examples:

```powershell
pwsh -File release/rtsp/scripts/run_rtsp_soak.ps1 -BuildDir build-mingw -DurationSec 3600 -Parallel 4 -Regex "rtsp" -OutDir .\logs\rtsp_soak
```

```bash
bash release/rtsp/scripts/run_rtsp_soak.sh build 3600 4 1 rtsp ./logs/rtsp_soak
```

### 3) Preflight (build + gate)

- Windows: `rtsp_preflight.ps1`
- Linux/macOS: `rtsp_preflight.sh`

Build required RTSP/RTCP test targets and run gate in one step.

### 4) Concurrency acceptance

- Windows: `run_rtsp_concurrency.ps1`
- Linux/macOS: `run_rtsp_concurrency.sh`

Run selected RTSP suites across multiple `ctest -j` levels and produce JSON summary.

Examples:

```powershell
pwsh -File release/rtsp/scripts/run_rtsp_concurrency.ps1 -BuildDir build-mingw -Regex "rtsp_server|rtsp_state_matrix|rtsp_interop" -ParallelLevels "2,4,8" -RoundsPerLevel 3
```

```bash
bash release/rtsp/scripts/run_rtsp_concurrency.sh build "rtsp_server|rtsp_state_matrix|rtsp_interop" "2,4,8" 3
```

## Environment Variables

- `YUAN_BUILD_DIR` (optional): override build directory for all scripts.

## Exit Codes

- `0`: success
- non-zero: failure (blocking)
