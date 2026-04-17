# Port Forwarding Tests

## 5.1 Local Forwarding (direct-tcpip)
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T5.1.1 | Open direct-tcpip channel | Valid target host:port | Channel opened | High |
| T5.1.2 | Target unreachable | Invalid target | CHANNEL_OPEN_FAILURE | High |

## 5.2 Remote Forwarding (tcpip-forward)
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T5.2.1 | Request tcpip-forward | bind_addr + port=0 | Allocated port returned | High |
| T5.2.2 | Cancel tcpip-forward | Previous bind addr + port | Forwarding cancelled | High |

## 5.3 Relay Data Path
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T5.3.1 | Bidirectional relay | Data from SSH client to target | Data forwarded to target, response returned | High |
| T5.3.2 | Relay with window flow control | Large data through direct-tcpip | Respects window/packet limits | Medium |
| T5.3.3 | Target connection closes | Remote end closes | CHANNEL_EOF sent to client | High |
| T5.3.4 | Client closes channel | Channel close mid-relay | Target connection cleaned up | High |
