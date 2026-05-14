# DNS Release Guide

## Goal

Provide a runnable DNS release target with:

- configurable records from JSON
- simple process management scripts
- smoke validation in CTest

## Build

```bash
cmake -S . -B build
cmake --build build --target release_dns_server
```

Output binary:

- `build/release/dns/release_dns_server`

## Run

```bash
build/release/dns/release_dns_server --config release/dns/config.json --port 5353
build/release/dns/release_dns_server --port 5353 --self-check-only
```

Or with helper scripts:

```bash
bash release/dns/start.sh
bash release/dns/stop.sh
```

PowerShell helpers:

```powershell
pwsh -File release/dns/start.ps1 -BuildDir build
pwsh -File release/dns/stop.ps1
```

## Config

Config file: `release/dns/config.json`

Example record entry:

```json
{
  "name": "mail.local",
  "type": "MX",
  "value": "10 mx1.mail.local"
}
```

Supported `type` values:

- `A`, `AAAA`, `TXT`, `CNAME`, `NS`, `MX`

Environment overrides:

- `YUAN_DNS_CONFIG`
- `YUAN_DNS_PORT`

## Validate

Run regression test:

```bash
ctest --test-dir build -R dns_regression --output-on-failure
```

Run release smoke test:

```bash
YUAN_RUN_RELEASE_DNS_SMOKE=1 ctest --test-dir build -R release_dns_server_smoke --output-on-failure
```

Run minimal release gate:

```bash
bash release/dns/gate.sh
```

```powershell
pwsh -File release/dns/gate.ps1 -BuildDir build -DnsPort 5353
```

Run CTest gate (opt-in):

```bash
YUAN_RUN_RELEASE_DNS_GATE=1 ctest --test-dir build -R release_dns_gate --output-on-failure
```

## Package

```bash
cmake --build build --target release_dns_package
```

Generated artifact:

- `build/release/packages/release_dns.zip`
- `build/release/packages/release_dns_1.0.0_<platform>_<arch>.zip`

Package contains `manifest.json` with version/platform/arch/commit/build time.
