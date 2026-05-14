# RTC/RTCP/WebRTC Implementation Plan

## Goal

- Validate the repository can build media transport foundations in-house.
- Implement protocol foundations in this order: `RTC (RTP-style packet)` -> `RTCP` -> `WebRTC integration`.
- Keep each stage independently testable and traceable.

## Scope and Milestones

### M1: RTC Packet Foundation (current)

- Add `protocol/rtc` module.
- Implement `RtcPacket` encode/decode:
  - fixed RTP-compatible header fields
  - CSRC list
  - extension header and extension payload
  - payload and optional padding
- Add protocol unit test target `test_rtc`.
- Acceptance:
  - header roundtrip passes
  - extension roundtrip passes
  - padding roundtrip passes

### M2: RTCP Packet Foundation (current)

- Add `protocol/rtcp` module.
- Implement `RtcpPacket` encode/decode for:
  - Sender Report (SR)
  - Receiver Report (RR)
  - BYE
  - report block codec
- Add protocol unit test target `test_rtcp`.
- Acceptance:
  - SR with report block roundtrip passes
  - RR with report blocks roundtrip passes
  - BYE with reason roundtrip passes

### M3: Session and Runtime Integration (next)

- Add session-level components on top of packet codecs:
  - `RtpSession`/`RtcpSession`
  - SSRC lifecycle and collision handling
  - sequence reorder and loss tracking
  - jitter and simple receive stats
- Add loopback and multi-stream tests.

Status update:

- `RtpSession` and `RtcpSession` base implementations are now added.
- Initial unit tests for session-level behavior are added.
- Next step is to extend from packet/session unit tests to loopback and multi-stream behavior tests.

### M4: WebRTC Transport Bridge (next)

- Reuse existing WebSocket service for signaling.
- Implement minimal signaling messages:
  - offer/answer
  - ICE candidate
- Integrate DTLS-SRTP and ICE stack (library-backed).
- Bridge encrypted RTP/RTCP to session modules.

### M5: Interop and Hardening (next)

- Browser interop with Chrome/Firefox.
- NAT and TURN scenarios.
- Stats and reliability:
  - RTCP NACK/PLI
  - key metrics and log points

## Implementation Notes

- Keep packet layers pure and dependency-light (`Core` only).
- Keep network I/O concerns out of codec classes.
- Prefer deterministic roundtrip tests before e2e networking tests.

## Current Delivery

- Delivered in this step:
  - `protocol/rtc` codec base + tests
  - `protocol/rtcp` codec base + tests
  - CMake wiring in main build and protocol test matrix
- Delivered in follow-up step:
  - `RtpSession` receive-side stats baseline (sequence tracking, loss estimate, jitter estimate)
  - `RtcpSession` SR/RR builder baseline
  - session-level tests: `test_rtp_session`, `test_rtcp_session`
