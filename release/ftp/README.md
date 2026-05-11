# release/ftp

This directory contains release-oriented runnable targets for FTP server and CLI.

## Binaries

- `release_ftp_server`: FTP server with config, env, and CLI option support
- `release_ftp_cli`: FTP client CLI for list, download, upload, append, and interactive mode

Default build output:

- `build/release/ftp/release_ftp_server`
- `build/release/ftp/release_ftp_cli`

## Server Config

Default config file:

- `release/ftp/config.json`

Key fields:

- `port`
- `root_dir`
- `passive_port_start` / `passive_port_end`
- `username` / `password`

Environment overrides:

- `YUAN_FTP_CONFIG`
- `YUAN_FTP_PORT`
- `YUAN_FTP_ROOT`
- `YUAN_FTP_USER`
- `YUAN_FTP_PASSWORD`
- `YUAN_FTP_PASV_START`
- `YUAN_FTP_PASV_END`

Server options:

```bash
build/release/ftp/release_ftp_server --config release/ftp/config.json --port 2121
build/release/ftp/release_ftp_server -f release/ftp/config.json --user admin --password s3cret
```

## Quick Run

Start server:

```bash
build/release/ftp/release_ftp_server --config release/ftp/config.json
```

Or use the release helper scripts after building:

```bash
bash release/ftp/start.sh
bash release/ftp/stop.sh
```

List files with bundled CLI:

```bash
build/release/ftp/release_ftp_cli --host 127.0.0.1 -p 2121 -u tester --password secret --list
```

Download/upload:

```bash
build/release/ftp/release_ftp_cli --host 127.0.0.1 -p 2121 -u tester --password secret --download remote.txt local.txt
build/release/ftp/release_ftp_cli --host 127.0.0.1 -p 2121 -u tester --password secret --upload local.txt remote.txt
```

Force active or passive data mode:

```bash
build/release/ftp/release_ftp_cli --force-active --host 127.0.0.1 -p 2121 --list
build/release/ftp/release_ftp_cli --force-passive --host 127.0.0.1 -p 2121 --download remote.txt local.txt
```

Interactive mode:

```bash
build/release/ftp/release_ftp_cli -i --host 127.0.0.1 -p 2121 -u tester --password secret
```

## Health Check

Script:

- `release/ftp/health_check.sh`

Run:

```bash
bash release/ftp/health_check.sh
```

Optional env:

- `BUILD_DIR`
- `SERVER_BIN`
- `CLI_BIN`
- `FTP_HOST`
- `FTP_PORT`
- `FTP_USER`
- `FTP_PASSWORD`
- `LOG_FILE`

Example:

```bash
FTP_HOST=127.0.0.1 FTP_PORT=2121 bash release/ftp/health_check.sh
```

## Build For Other Linux Machines

Default build links `libstdc++/libgcc` statically for portability across similar Linux distributions.

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

- `build-musl-release/release/ftp`

## Supported FTP Commands

Server:

- USER, PASS, QUIT, SYST, FEAT, HELP, NOOP
- PASV, EPSV, PORT, EPRT
- LIST, NLST, RETR, STOR, APPE, STOU
- CWD, PWD, MKD, RMD, CDUP
- DELE, RNFR, RNTO
- SIZE, TYPE, ALLO, REST, STAT
- ABOR

Client:

- Auto/EPSV+PASV passive or EPRT+PORT active data mode
- LIST, NLST, RETR, STOR, APPE
- Interactive shell mode