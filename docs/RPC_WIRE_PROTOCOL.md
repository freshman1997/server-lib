# Yuan RPC Wire Protocol

This document defines the stable binary RPC frame used by `libs/rpc`. It is a general-purpose service RPC frame for clients, servers, tools, and backend services implemented in different languages.

## Goals

- Fixed-size header for fast routing and early validation.
- Big-endian integer encoding for portable multi-language parsing.
- Payload-agnostic body so clients can use JSON, Protobuf, FlatBuffers, MsgPack, or raw bytes.
- Request, response, and push frames in one format.
- Stream-friendly framing for TCP, WebSocket binary frames, KCP, or QUIC streams.

## Byte Order

All integer fields are unsigned and encoded in network byte order, big-endian.

## Header

The header is exactly 68 bytes.

| Offset | Size | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 4 | magic | u32 | `0x59525043`, ASCII `YRPC` |
| 4 | 1 | version | u8 | Current version is `1` |
| 5 | 1 | kind | u8 | `0=request`, `1=response`, `2=push` |
| 6 | 1 | serialization | u8 | `0=raw`, `1=json`, `2=protobuf`, `3=flatbuffers`, `4=msgpack` |
| 7 | 1 | compression | u8 | `0=none`, `1=zstd`, `2=lz4`, `3=gzip` |
| 8 | 2 | status | u16 | Response status. Requests and pushes use `0` |
| 10 | 2 | encryption | u16 | `0=none`, `1=xor_stream`, `2=aes_128_gcm`, `3=aes_256_gcm`, `4=chacha20_poly1305` |
| 12 | 4 | body_size | u32 | Size of the body following this header |
| 16 | 8 | request_id | u64 | Correlates request and response. Push may use `0` |
| 24 | 4 | service | u32 | Numeric service id. May be `0` when using route name |
| 28 | 4 | method | u32 | Numeric method id. May be `0` when using route name |
| 32 | 4 | route_name_size | u32 | Route name bytes in body |
| 36 | 4 | metadata_size | u32 | Metadata bytes in body |
| 40 | 4 | error_size | u32 | Error string bytes in body. Responses only |
| 44 | 4 | payload_size | u32 | Payload bytes in body |
| 48 | 8 | nonce | u64 | Encryption nonce or `0` |
| 56 | 8 | continuation_id | u64 | Async continuation id. `0` means unused. Kept wire-compatible with older `coroutine_id` naming |
| 64 | 4 | key_id | u32 | Encryption key id or `0` |

## Body

When `encryption=0`, body sections are concatenated in this order:

| Section | Size Field | Encoding |
|---|---|---|
| route_name | `route_name_size` | UTF-8 bytes |
| metadata | `metadata_size` | Metadata map encoding below |
| error | `error_size` | UTF-8 bytes |
| payload | `payload_size` | Raw serialized payload bytes |

When encryption is enabled, the same plaintext body layout is encrypted as one contiguous body. Header fields remain unencrypted so routers can validate, rate limit, and dispatch by service/method.

## Metadata Encoding

Metadata is a compact string map:

| Field | Type | Description |
|---|---|---|
| count | u16 | Number of key-value pairs |
| key_len | u16 | Key byte length |
| key | bytes | UTF-8 key |
| value_len | u16 | Value byte length |
| value | bytes | UTF-8 value |

The key/value pair sequence repeats `count` times.

## Status Values

| Value | Name |
|---:|---|
| 0 | ok |
| 1 | not_found |
| 2 | timeout |
| 3 | canceled |
| 4 | bad_request |
| 5 | unavailable |
| 6 | internal_error |

## Decoder Rules

- Reject frames with invalid magic.
- Reject unsupported version.
- If fewer than 68 bytes are available, wait for more data.
- If fewer than `68 + body_size` bytes are available, wait for more data.
- Reject frames above the configured max frame size.
- For unencrypted frames, `route_name_size + metadata_size + error_size + payload_size` must equal `body_size`.
- For encrypted frames, decrypt body first, then the decrypted body size must equal the section size sum.

## Multi-Language Parser Skeleton

```text
read 68-byte header
validate magic/version
read body_size
read body bytes
if encryption != 0: body = decrypt(header, body)
route_name = body[0:route_name_size]
metadata = parse_metadata(next metadata_size bytes)
error = next error_size bytes as UTF-8
payload = next payload_size bytes
```

## Payload Choice

The RPC layer does not inspect payload content. The `serialization` field tells the client which codec to use:

- `raw`: binary bytes or plain text by application convention.
- `json`: UTF-8 JSON document.
- `protobuf`: Protobuf message bytes.
- `flatbuffers`: FlatBuffers buffer bytes.
- `msgpack`: MessagePack document bytes.

## Async Continuation Resume

`request_id` is the network-level RPC correlation id. `continuation_id` is the local async continuation resume id. In C++ this can map to a suspended coroutine; in other languages it can map to a promise, future, callback token, fiber, green thread, or actor mailbox entry.

Recommended usage:

- Client or server runtime creates a pending continuation and writes its id to `continuation_id`.
- The request sender still uses `request_id` for RPC pending-call matching.
- The responder copies `continuation_id` back unchanged.
- When response arrives, business code can resume the waiting continuation by `continuation_id`.

The C++ API still exposes `coroutine_id` as a compatibility alias because the field was originally introduced for C++20 coroutine resume. Prefer `ContinuationId` and `continuation_id()` in new generic code.
