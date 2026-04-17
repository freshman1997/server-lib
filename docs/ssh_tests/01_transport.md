# Transport Layer Tests

## 1.1 Version Exchange
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T1.1.1 | Valid client version | `SSH-2.0-OpenSSH_8.9` | Parsed correctly, version_exchanged state | High |
| T1.1.2 | Invalid protocol version | `SSH-1.5-Client` | Session disconnected | High |
| T1.1.3 | Missing version line | Binary garbage | Session disconnected | High |
| T1.1.4 | Version with comments | `SSH-2.0-Client comment here` | Parsed, comment ignored | Medium |

## 1.2 Algorithm Negotiation (KEXINIT)
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T1.2.1 | Overlapping algorithms | Client supports curve25519-sha256 | Negotiated successfully | High |
| T1.2.2 | No overlapping kex | Client only supports unknown-kex | Session disconnected | High |
| T1.2.3 | No overlapping cipher | Client only supports unknown-cipher | Session disconnected | High |
| T1.2.4 | Algorithm ordering preference | Client prefers ecdh-sha2-nistp521 | Server's first match wins per RFC | Medium |
| T1.2.5 | Default registry completeness | No custom algorithms | All 8 kex, 6 cipher, 3 mac, 3 compression registered | High |

## 1.3 Key Exchange (DH/ECDH)
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T1.3.1 | ECDH P256 exchange | Valid client public key | KEX reply + NEWKEYS sent | High |
| T1.3.2 | Curve25519 exchange | Valid client public key | KEX reply + NEWKEYS sent | High |
| T1.3.3 | DH Group14 exchange | Valid client public key | KEX reply + NEWKEYS sent | High |
| T1.3.4 | Invalid client public | Zero-length key | Session disconnected | High |
| T1.3.5 | Session ID stability | Two KEX rounds | session_id unchanged after rekey | Medium |

## 1.4 Encryption/Decryption
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T1.4.1 | AES-256-CTR roundtrip | Encrypted packet | Decrypted payload matches | High |
| T1.4.2 | AES-128-GCM roundtrip | AEAD encrypted packet | Decrypted + MAC verified | High |
| T1.4.3 | Chacha20-Poly1305 roundtrip | AEAD encrypted packet | Decrypted + MAC verified | High |
| T1.4.4 | MAC error detection | Tampered packet | SSH_DISCONNECT_MAC_ERROR | High |
| T1.4.5 | Sequence number increment | 10 consecutive packets | seq 1..10 used in MAC | Medium |
