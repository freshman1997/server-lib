# release/ssh

This directory contains release-oriented runnable targets for SSH server and CLI.

## Binaries

- `release_ssh_server`: SSH server entry with config, env, and CLI option support
- `release_ssh_cli`: OpenSSH-shaped client CLI for probe and password-auth exec

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
- `enable_port_forwarding`

Environment overrides (subset):

- `YUAN_SSH_CONFIG`
- `YUAN_SSH_PORT`
- `YUAN_SSH_HOST_KEY`
- `YUAN_SSH_USER`
- `YUAN_SSH_PASSWORD`
- `YUAN_SSH_ENABLE_PUBLICKEY_AUTH`
- `YUAN_SSH_ENABLE_PASSWORD_AUTH`
- `YUAN_SSH_AUTHORIZED_KEYS`
- `YUAN_SSH_ENABLE_SFTP`
- `YUAN_SSH_ENABLE_PORT_FORWARD`

Server options:

```bash
build/release/ssh/release_ssh_server --config release/ssh/config.json --port 2222
build/release/ssh/release_ssh_server -f release/ssh/config.json --password-auth yes --sftp yes
```

## Quick Run

Start server:

```bash
build/release/ssh/release_ssh_server --config release/ssh/config.json
```

Or use the release helper scripts after building:

```bash
build/release/ssh/start.sh
build/release/ssh/stop.sh
```

Probe with bundled CLI:

```bash
build/release/ssh/release_ssh_cli --probe -p 2222 127.0.0.1
```

The bundled CLI intentionally accepts OpenSSH-shaped syntax:

```bash
release_ssh_cli [options] [user@]host [command]
release_ssh_cli --probe -p 2222 yuan@127.0.0.1
release_ssh_cli -p 2222 --password yuan yuan@127.0.0.1 "whoami"
release_ssh_cli -o Port=2222 -o User=yuan --probe 127.0.0.1
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

`release_ssh_cli` currently supports:

- TCP connect and SSH version probe via `--probe`
- key exchange negotiation
- password authentication
- non-interactive `exec` command channels
- OpenSSH-shaped options: `-p`, `-l`, `-i`, `-o`, `-F`, `-q`, `-v`, `-V`

It does **not** yet implement:

- interactive shell mode when no command is provided
- publickey authentication from `-i`
- known_hosts verification
- local config-file parsing from `-F`

For interactive shell/session validation, use OpenSSH client (`ssh`).
