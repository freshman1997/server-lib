# RTSP Release Checklist

## 1. Scope Freeze

- Release owner:
- Target commit:
- Release date:
- Scope notes:

## 2. Required Commands

### 2.1 Preflight

- Windows:

```powershell
pwsh -File release/rtsp/scripts/rtsp_preflight.ps1
```

- Linux/macOS:

```bash
bash release/rtsp/scripts/rtsp_preflight.sh
```

### 2.2 Gate (blocking)

- Windows:

```powershell
pwsh -File release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex "rtsp|rtcp"
```

- Linux/macOS:

```bash
bash release/rtsp/scripts/run_rtsp_gate.sh build "rtsp|rtcp"
```

### 2.3 Soak

- Windows:

```powershell
pwsh -File release/rtsp/scripts/run_rtsp_soak.ps1 -BuildDir build-mingw -DurationSec 86400 -Parallel 4 -Regex "rtsp" -OutDir .\logs\rtsp_soak
```

- Linux/macOS:

```bash
bash release/rtsp/scripts/run_rtsp_soak.sh build 86400 4 1 rtsp ./logs/rtsp_soak
```

## 3. Pass Criteria (Release Blocking)

- Gate `rtsp|rtcp`: 100% pass.
- Soak fail count: 0.
- No crash/hang in logs.
- Outbound UDP drop ratio within accepted threshold.
- Digest auth regressions: pass.

## 4. Artifacts to Attach

- Gate logs.
- Soak `driver-*.log` / `iterations-*.jsonl` / `summary-*.json`.
- Final ctest output.
- UDP/NAT validation record.

## 5. Risk Sign-off

- Protocol owner:
- Test owner:
- Ops owner:
- Final decision: PASS / HOLD
