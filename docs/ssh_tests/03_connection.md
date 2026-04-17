# Connection Layer Tests

## 3.1 Channel Management
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T3.1.1 | Open session channel | `session` type | CHANNEL_OPEN_CONFIRMATION | High |
| T3.1.2 | Open direct-tcpip | `direct-tcpip` type | CHANNEL_OPEN_CONFIRMATION | High |
| T3.1.3 | Channel limit | 65th channel | CHANNEL_OPEN_FAILURE (resource shortage) | High |
| T3.1.4 | Unknown channel type | `unknown` type | CHANNEL_OPEN_FAILURE | High |
| T3.1.5 | Close channel | CHANNEL_CLOSE | CHANNEL_CLOSE echoed | High |

## 3.2 Window Flow Control
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T3.2.1 | Window adjust sent | Consume window below threshold | WINDOW_ADJUST sent back | High |
| T3.2.2 | Data exceeds window | Data larger than available window | Data rejected | High |
| T3.2.3 | Data exceeds max packet | Single data > max_packet_size | Data rejected | Medium |
| T3.2.4 | Remote window adjust | WINDOW_ADJUST received | remote_window increased | High |

## 3.3 Channel Requests
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T3.3.1 | Subsystem request (sftp) | `subsystem` `sftp` | CHANNEL_SUCCESS, handler set | High |
| T3.3.2 | Subsystem request (unknown) | `subsystem` `unknown` | CHANNEL_FAILURE | High |
| T3.3.3 | Exec request | `exec` `ls -la` | Handler callback invoked | Medium |
| T3.3.4 | PTY request | `pty-req` with terminal info | Handler callback invoked | Medium |
| T3.3.5 | Shell request | `shell` | Handler callback invoked | Medium |
| T3.3.6 | Env request | `env` `TERM=xterm` | Handler callback invoked | Low |
| T3.3.7 | Signal | `signal` `TERM` | Handler callback invoked | Low |
| T3.3.8 | Window change | `window-change` 80x24 | Handler callback invoked | Low |
| T3.3.9 | Request without reply | want_reply=false | No response sent | Medium |

## 3.4 Channel Data (Write-Back Path)
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T3.4.1 | SFTP response routed | SFTP enqueues data on channel | SSH_MSG_CHANNEL_DATA sent to client | High |
| T3.4.2 | Multiple channels pending | 2 channels with pending data | Both channels flushed | High |
| T3.4.3 | Window limit on send | Pending data > remote_window | Only window-sized data sent, rest queued | High |
| T3.4.4 | Max packet splitting | Pending > remote_max_packet | Data split into max_packet chunks | Medium |

## 3.5 Global Requests
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T3.5.1 | tcpip-forward | bind_addr + port | REQUEST_SUCCESS with allocated port | High |
| T3.5.2 | cancel-tcpip-forward | bind_addr + port | REQUEST_SUCCESS | High |
| T3.5.3 | Unknown global request | Unknown name | REQUEST_FAILURE (if want_reply) | Medium |
| T3.5.4 | No reply requested | want_reply=false | No response | Medium |
