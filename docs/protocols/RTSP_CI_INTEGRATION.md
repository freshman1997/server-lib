# RTSP CI Integration Guide

## 1) Minimal Gate Command

Script index:

- `release/rtsp/scripts/README.md`

- Gate script: `release/rtsp/scripts/run_rtsp_gate.ps1`
- Command:

```powershell
pwsh -File release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex "rtsp|rtcp"
```

Linux/macOS:

```bash
bash release/rtsp/scripts/run_rtsp_gate.sh build "rtsp|rtcp"
```

## 2) Jenkins (Declarative Pipeline)

```groovy
pipeline {
  agent any
  stages {
    stage('Build RTSP') {
      steps {
        bat 'cmake --build build-mingw --target test_rtsp_server test_rtsp_interop test_rtcp test_rtcp_session test_rtcp_loopback'
      }
    }
    stage('RTSP Gate') {
      steps {
        bat 'pwsh -File release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex "rtsp|rtcp"'
      }
    }
  }
}
```

## 3) GitLab CI

```yaml
rtsp_gate:
  stage: test
  script:
    - cmake --build build-mingw --target test_rtsp_server test_rtsp_interop test_rtcp test_rtcp_session test_rtcp_loopback
    - pwsh -File release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex "rtsp|rtcp"
```

## 4) GitHub Actions

```yaml
name: rtsp-gate
on: [pull_request]
jobs:
  gate:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build RTSP targets
        run: cmake --build build-mingw --target test_rtsp_server test_rtsp_interop test_rtcp test_rtcp_session test_rtcp_loopback
      - name: Run RTSP gate
        shell: pwsh
        run: ./release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex "rtsp|rtcp"
```

## 5) Recommended Policy

- PR required check: `rtsp-gate` must pass.
- Release branch: run gate + soak summary artifact.
