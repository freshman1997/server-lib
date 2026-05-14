# RTSP UDP NAT Validation Template

## 1. Environment

- Date:
- Build/commit:
- Server host/network:
- Client host/network:
- NAT type:

## 2. Scenario Matrix

- UDP unicast `SETUP/PLAY` single track
- UDP unicast multi-track
- Mixed TCP+UDP same session
- Session timeout/reconnect under NAT

## 3. Capture Points

- Server logs (RTSP + outbound metrics)
- Client-side packet capture (rtcp receive)
- NAT/firewall logs (if available)

## 4. Checks

- Server advertises correct `server_port`
- Client `client_port` is accepted and used for RTCP send-back
- `outbound_udp_sent` increases on steady state
- `outbound_udp_failed/retried/dropped` remain within expected range

## 5. Result

- PASS/FAIL per scenario
- Known issues
- Follow-up actions
