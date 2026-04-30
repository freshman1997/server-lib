# release/ssh

This directory contains release-oriented runnable targets for SSH server and CLI.

## Binaries

- `release_ssh_server`: SSH server entry with config/env support
- `release_ssh_cli`: minimal connectivity/version-exchange client

Default build output:

- `build/release/ssh/release_ssh_server`
- `build/release/ssh/release_ssh_cli`

## Server Config

Default config file:

- `release/ssh/config.json`

Key fields:

- `port`
- `host_key_path`
- `username` / `password`
- `enable_publickey_auth`
- `enable_password_auth`
- `enable_sftp`

Environment overrides (subset):

- `YUAN_SSH_CONFIG`
- `YUAN_SSH_PORT`
- `YUAN_SSH_HOST_KEY`
- `YUAN_SSH_USER`
- `YUAN_SSH_PASSWORD`
- `YUAN_SSH_ENABLE_PUBLICKEY_AUTH`
- `YUAN_SSH_ENABLE_PASSWORD_AUTH`
- `YUAN_SSH_AUTHORIZED_KEYS`

## Quick Run

Start server:

```bash
build/release/ssh/release_ssh_server
```

Connect with system ssh client:

```bash
ssh -p 2222 <user>@127.0.0.1
```

## Health Check

Script:

- `release/ssh/health_check.sh`

Run:

```bash
bash release/ssh/health_check.sh
```

Optional env to avoid machine-specific assumptions:

- `BUILD_DIR`
- `SERVER_BIN`
- `CLI_BIN`
- `SSH_HOST`
- `SSH_PORT`
- `SSH_USER`
- `SSH_KEY_PATH`
- `LOG_FILE`
- `KNOWN_HOSTS_FILE`

Example:

```bash
SSH_HOST=127.0.0.1 SSH_PORT=2222 bash release/ssh/health_check.sh
```

## Build For Other Linux Machines

Default `build_release.sh` output is glibc-based and now links `libstdc++/libgcc` statically.
It is usually portable across similar Linux distributions but still depends on glibc.

For maximum portability, build a musl static package:

```bash
bash build_release_musl.sh
```

Requirements:

- `x86_64-linux-musl-gcc`
- `x86_64-linux-musl-g++`

Toolchain file:

- `cmake/toolchains/linux-musl-static.cmake`

Musl output directory:

- `build-musl-release/release/ssh`

## Current CLI Scope

`release_ssh_cli` currently verifies transport reachability and SSH version exchange only.

It does **not** implement:

- key exchange negotiation
- user authentication
- encrypted channel/session interaction

For interactive shell/session validation, use OpenSSH client (`ssh`).
