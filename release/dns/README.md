# release/dns

This directory contains release-oriented runnable assets for DNS server.

## Binary

- `release_dns_server`: DNS UDP server with config, env, and CLI option support

Default build output:

- `build/release/dns/release_dns_server`

## Server Config

Default config file:

- `release/dns/config.json`

Key fields:

- `port`
- `records` (array of `{name, type, value}`)

Supported record types:

- `A`
- `AAAA`
- `TXT`
- `CNAME`
- `NS`
- `MX` (value format: `"<preference> <exchange>"`)

Environment overrides:

- `YUAN_DNS_CONFIG`
- `YUAN_DNS_PORT`

Server options:

```bash
build/release/dns/release_dns_server --config release/dns/config.json --port 5353
build/release/dns/release_dns_server -f release/dns/config.json
build/release/dns/release_dns_server --port 5353 --self-check-only
```

## Quick Run

Start server:

```bash
build/release/dns/release_dns_server --config release/dns/config.json
```

Or use helper scripts after building:

```bash
bash release/dns/start.sh
bash release/dns/stop.sh
```

Windows PowerShell helpers:

```powershell
pwsh -File release/dns/start.ps1 -BuildDir build
pwsh -File release/dns/stop.ps1
```

## Health Check

Script:

- `release/dns/health_check.sh`
- `release/dns/health_check.ps1`

Run:

```bash
bash release/dns/health_check.sh
```

```powershell
pwsh -File release/dns/health_check.ps1 -BuildDir build -DnsPort 5353
```

Optional env:

- `BUILD_DIR`
- `SERVER_BIN`
- `DNS_PORT`

## E2E Gate

Linux/macOS:

```bash
bash release/dns/gate.sh
```

Windows PowerShell:

```powershell
pwsh -File release/dns/gate.ps1 -BuildDir build -DnsPort 5353
```

CTest gate (opt-in):

```bash
YUAN_RUN_RELEASE_DNS_GATE=1 ctest --test-dir build -R release_dns_gate --output-on-failure
```

## Package

Create zip package:

```bash
cmake --build build --target release_dns_package
```

Output:

- `build/release/packages/release_dns.zip`
- `build/release/packages/release_dns_1.0.0_windows_x86_64.zip` (example)

Package includes:

- `manifest.json` (version/platform/arch/commit/build time)

## Smoke Test

`ctest` target:

- `release_dns_server_smoke`

Enable and run:

```bash
YUAN_RUN_RELEASE_DNS_SMOKE=1 ctest --test-dir build -R release_dns_server_smoke --output-on-failure
```
