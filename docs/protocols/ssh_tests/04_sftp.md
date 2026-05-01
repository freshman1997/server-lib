# SFTP Subsystem Tests

## 4.1 Version Negotiation
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.1.1 | Client v3 | SSH_FXP_INIT version=3 | SSH_FXP_VERSION version=3 | High |
| T4.1.2 | Client v6 | SSH_FXP_INIT version=6 | SSH_FXP_VERSION version=3 (negotiated down) | High |
| T4.1.3 | Init before version | Any other packet | Ignored | Medium |

## 4.2 File Operations
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.2.1 | Open file for read | SSH_FXF_READ | Handle returned | High |
| T4.2.2 | Open file for write | SSH_FXF_WRITE \| SSH_FXF_CREAT | Handle returned | High |
| T4.2.3 | Open non-existent (read) | SSH_FXF_READ, no file | SSH_FX_NO_SUCH_FILE | High |
| T4.2.4 | Open with O_EXCL | SSH_FXF_CREAT \| SSH_FXF_EXCL, file exists | SSH_FX_FILE_ALREADY_EXISTS | Medium |
| T4.2.5 | Read file | Valid handle + offset | SSH_FXP_DATA returned | High |
| T4.2.6 | Read past EOF | offset > file_size | SSH_FX_EOF | High |
| T4.2.7 | Write file | Valid handle + data | SSH_FX_OK | High |
| T4.2.8 | Close file | Valid handle | SSH_FX_OK | High |
| T4.2.9 | Close invalid handle | Bad handle string | SSH_FX_INVALID_HANDLE | High |

## 4.3 Directory Operations
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.3.1 | Open directory | Valid dir path | Handle returned | High |
| T4.3.2 | Read directory | Valid dir handle | SSH_FXP_NAME with entries | High |
| T4.3.3 | Read directory EOF | After all entries read | SSH_FX_EOF | High |
| T4.3.4 | Open non-directory | Regular file path | SSH_FX_NOT_A_DIRECTORY | Medium |

## 4.4 Attribute Operations
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.4.1 | lstat | Valid path | SSH_FXP_ATTRS returned | High |
| T4.4.2 | stat (follow links) | Symlink path | Target's attrs | High |
| T4.4.3 | fstat | Valid file handle | SSH_FXP_ATTRS returned | High |
| T4.4.4 | setstat | path + new permissions | SSH_FX_OK | Medium |
| T4.4.5 | fsetstat | handle + new size | SSH_FX_OK | Medium |

## 4.5 Path Operations
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.5.1 | realpath | Relative path `./foo` | Canonical absolute path | High |
| T4.5.2 | remove | Existing file | SSH_FX_OK | High |
| T4.5.3 | remove non-existent | Missing file | SSH_FX_NO_SUCH_FILE | High |
| T4.5.4 | mkdir | New dir path | SSH_FX_OK | High |
| T4.5.5 | mkdir existing | Already exists | SSH_FX_FILE_ALREADY_EXISTS | Medium |
| T4.5.6 | rmdir | Empty dir | SSH_FX_OK | High |
| T4.5.7 | rmdir non-empty | Dir with files | SSH_FX_DIR_NOT_EMPTY | Medium |
| T4.5.8 | rename | old_path + new_path | SSH_FX_OK | High |
| T4.5.9 | rename non-existent | Missing old_path | SSH_FX_NO_SUCH_FILE | Medium |
| T4.5.10 | readlink | Symlink path | SSH_FXP_NAME with target | High |
| T4.5.11 | symlink | link_path + target_path | SSH_FX_OK | High |

## 4.6 Path Sandboxing
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.6.1 | Path traversal | `/../../etc/passwd` | Resolved within root_dir | High |
| T4.6.2 | Relative path | `foo/bar` | Rejected (must start with /) | High |
| T4.6.3 | Root dir config | sftp_root_dir="/home/user" | All paths confined | High |

## 4.7 Packet Framing
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.7.1 | Single complete packet | 4-byte length + payload | Decoded correctly | High |
| T4.7.2 | Partial packet | First 3 bytes only | Buffered, wait for more | High |
| T4.7.3 | Multiple packets in one read | Two packets concatenated | Both decoded | High |
| T4.7.4 | Large read response | 32KB read | Data chunked if needed | Medium |

## 4.8 Extended Operations
| # | Test Case | Input | Expected | Priority |
|---|-----------|-------|----------|----------|
| T4.8.1 | Unknown extended | Any extension name | SSH_FX_OP_UNSUPPORTED | High |
