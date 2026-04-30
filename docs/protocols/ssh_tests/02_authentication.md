# Authentication Layer Tests

## 2.1 Service Request
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T2.1.1 | Request userauth service | `ssh-userauth` | SERVICE_ACCEPT sent | High |
| T2.1.2 | Request connection before auth | `ssh-connection` (not authed) | Rejected | High |
| T2.1.3 | Request connection after auth | `ssh-connection` (authed) | auth_success state | High |

## 2.2 Password Authentication
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T2.2.1 | Valid password | Correct credentials | auth_success | High |
| T2.2.2 | Invalid password | Wrong credentials | USERAUTH_FAILURE | High |
| T2.2.3 | Max attempts exceeded | 7 failed attempts | Session disconnected | High |
| T2.2.4 | Partial success with next method | Password ok but require pubkey | partial_success=true in failure | Medium |

## 2.3 Public Key Authentication
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T2.3.1 | Probe (no signature) | has_signature=false | USERAUTH_PK_OK | High |
| T2.3.2 | Valid signature | Correct signature | auth_success | High |
| T2.3.3 | Invalid signature | Bad signature | USERAUTH_FAILURE | High |
| T2.3.4 | Unknown key | Unrecognized public key | USERAUTH_FAILURE | High |
| T2.3.5 | Wrong algorithm | Non-matching algo name | USERAUTH_FAILURE | Medium |

## 2.4 Keyboard-Interactive Authentication
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T2.4.1 | Info request sent | Initial request | USERAUTH_INFO_REQUEST with prompts | High |
| T2.4.2 | Valid response | Correct answer | auth_success | High |
| T2.4.3 | Invalid response | Wrong answer | USERAUTH_FAILURE | High |
| T2.4.4 | NEED_MORE state | After INFO_REQUEST | auth_need_more state | Medium |

## 2.5 Auth Method Negotiation
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T2.5.1 | Failure lists methods | Any auth failure | methods_that_can_continue populated | High |
| T2.5.2 | Disable method | config.auth_methods = {"publickey"} | Password returns failure | Medium |
| T2.5.3 | Auth timeout | No auth for 60s | Session disconnected (if timeout configured) | Low |
