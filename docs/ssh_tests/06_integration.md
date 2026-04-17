# Integration Tests

## 6.1 Full Session Flow
| # | Test Case | Steps | Expected | Priority |
|---|-----------|-------|----------|----------|
| T6.1.1 | Complete SSH handshake | Version → KEXINIT → KEX → NEWKEYS | Encrypted session established | High |
| T6.1.2 | Auth + channel open | Handshake → auth → open channel | Channel ready | High |
| T6.1.3 | Full SFTP session | Handshake → auth → sftp → operations → close | All ops succeed | High |
| T6.1.4 | Multiple concurrent sessions | 10 simultaneous connections | All sessions independent | High |
| T6.1.5 | Session limit | max_sessions+1 connections | Last connection rejected | Medium |

## 6.2 Real Client Compatibility
| # | Test Case | Client | Expected | Priority |
|---|-----------|--------|----------|----------|
| T6.2.1 | OpenSSH sftp | `sftp -P 22 user@localhost` | Connection + file ops | High |
| T6.2.2 | OpenSSH scp | `scp file user@localhost:/tmp/` | File transferred | High |
| T6.2.3 | OpenSSH port forward | `ssh -L 8080:host:80` | Forwarding works | High |
| T6.2.4 | WinSCP | WinSCP client | Connection + file ops | Medium |
| T6.2.5 | FileZilla | FileZilla SFTP | Connection + file ops | Medium |

## 6.3 Error Handling
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T6.3.1 | Connection drop | Close socket mid-session | Clean cleanup, no leak | High |
| T6.3.2 | Invalid packet | Malformed SSH packet | Disconnect sent | High |
| T6.3.3 | Idle timeout | No data for timeout_ms | Session cleaned up | Medium |
| T6.3.4 | Channel cleanup on session close | Session with open channels | All channels closed | High |

## 6.4 Performance
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T6.4.1 | Large file download | 1GB file via SFTP | Completed without error | High |
| T6.4.2 | Large file upload | 1GB file via SFTP | Completed without error | High |
| T6.4.3 | Many small files | 10000 files readdir | No handle leak | Medium |
| T6.4.4 | Concurrent sessions | 100 simultaneous sessions | All succeed | Medium |
| T6.4.5 | Zero-copy read path | pread via SshLocalFileSystem | No unnecessary memcpy | Low |

## 6.5 Rekey
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T6.5.1 | Client-initiated rekey | KEXINIT during active session | Rekey completes, session continues | High |
| T6.5.2 | Rekey during data transfer | KEXINIT while SFTP active | Data resumes after rekey | High |
| T6.5.3 | Rekey failure | Bad KEX data during rekey | Session disconnected cleanly | Medium |
