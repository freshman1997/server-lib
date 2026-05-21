# Proxy Service Production Readiness

This checklist is for `ProxyService` HTTP proxy deployment readiness.

## 1) Functional Gates (must pass)

- `test_proxy_service_integration` passes with zero failures.
- Browser traffic works for both short requests and long media streams.
- Service accepts and drains cleanly (`stop()` does not leave active sessions behind).

Suggested command:

```powershell
cmake --build build --target test_proxy_service_integration -j 4
ctest --test-dir build -R proxy_service_integration --output-on-failure
```

## 2) Resource & Leak Gates (must pass)

Use the per-second aggregate log line:

`[ProxyService] traffic aggregate 1s active=... up_Bps=... down_Bps=... total_up=... total_down=... tunnel_mem=...B process_mem=...B`

Pass criteria:

- During load: `up_Bps/down_Bps` are non-zero and stable.
- After client stop: `active` returns to `0`.
- After client stop: `up_Bps/down_Bps` return to `0`.
- After an idle window: `tunnel_mem` returns near `0`.
- `process_mem` may not fully drop (allocator behavior), but it should not keep growing every idle second.

## 3) Soak Gates (recommended)

- 30-minute soak: no crashes, no stuck active sessions, no monotonic idle memory growth.
- 2-hour soak: same as above.
- 24-hour soak before production rollout.

## 4) Security Gates (must pass)

- If enabled, basic auth rejects invalid credentials.
- ACL/deny/allow target rules behave as expected.
- Private target policy is explicitly set for your environment.
- Logs do not expose sensitive credentials.

## 5) Operational Gates (must pass)

- Startup/stop procedures documented.
- Alerting on repeated errors/timeouts.
- Log retention and rotation configured.
- Capacity limits (`max_active_sessions`, buffer limits) tuned for hardware.

## Quick Runbook

1. Run integration tests.
2. Run `run_proxy_soak.ps1` for 30+ minutes.
3. Review `traffic aggregate 1s` lines in the generated log.
4. Confirm idle convergence (`active=0`, throughput=0, no idle memory climb).
