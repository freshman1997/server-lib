# release/ssh

This directory contains release-oriented runnable targets for SSH server and CLI.

## Binaries

- `release_ssh_server`: SSH server entry with config, env, and CLI option support
- `release_ssh_cli`: OpenSSH-shaped client CLI for probe, exec, shell, publickey/password auth, and forwarding smoke use

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
release_ssh_cli --stderr-prefix -p 2222 --password yuan yuan@127.0.0.1 "echo out & dir nosuchfile"
```

Interactive shell:

```bash
release_ssh_cli -p 2222 --password yuan yuan@127.0.0.1
```

Public key authentication:

```bash
release_ssh_cli -p 2222 -i ~/.ssh/id_ed25519 yuan@127.0.0.1 "whoami"
```

Forwarding examples:

```bash
release_ssh_cli -p 2222 --password yuan -L 127.0.0.1:8080:127.0.0.1:80 yuan@127.0.0.1
release_ssh_cli -p 2222 --password yuan -D 127.0.0.1:1080 yuan@127.0.0.1
release_ssh_cli -p 2222 --password yuan -R 127.0.0.1:9000:127.0.0.1:80 yuan@127.0.0.1
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

## Windows (MinGW)

SSH targets are available on Windows when building with MinGW.

Configure and build:

```bash
cmake -S . -B build-win-mingw-ssh -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DYUAN_ENABLE_SSH=ON -DYUAN_ENABLE_SSH_SFTP=ON
cmake --build build-win-mingw-ssh --target release_ssh_cli release_ssh_server -j 8
```

Output binaries:

- `build-win-mingw-ssh/release/ssh/release_ssh_cli.exe`
- `build-win-mingw-ssh/release/ssh/release_ssh_server.exe`

Quick verify:

```bash
build-win-mingw-ssh/release/ssh/release_ssh_cli.exe --version
```

## Current CLI Scope

`release_ssh_cli` currently supports:

- TCP connect and SSH version probe via `--probe`
- key exchange negotiation
- password authentication
- publickey authentication from `-i`
- known_hosts verification/update with strict host key policy options
- non-interactive `exec` command channels
- interactive shell mode when no command is provided
- PTY allocation for interactive TTY stdin, including terminal size, terminal modes, window-change, and SIGINT forwarding on Linux
- no-PTY shell mode for piped stdin, matching OpenSSH's default behavior for non-TTY stdin
- local forwarding via `-L`
- dynamic SOCKS5 forwarding via `-D`
- remote forwarding via `-R`
- OpenSSH-shaped options: `-p`, `-l`, `-i`, `-o`, `-F`, `-q`, `-v`, `-V`

Known limits:

- `-F` is accepted for OpenSSH-shaped compatibility, but full ssh_config file parsing is intentionally limited.
- The bundled CLI is a release smoke/utility client, not a full OpenSSH replacement.
- SFTP is supported by the server and validated with OpenSSH `sftp`; the bundled CLI does not implement an interactive SFTP client.
- ZMODEM/lrzsz (`sz`/`rz`) is not built in. PTY sessions pass bytes through, but use SFTP or forwarding for reliable file transfer.

For deeper interoperability validation, use OpenSSH client tools (`ssh`, `sftp`) against `release_ssh_server`.
