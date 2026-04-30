# SSH CLI Usage

`ssh_cli_demo` is a lightweight first-party SSH CLI binary built on top of `libs/ssh_cli`.

## Build

```bash
cmake -S . -B build
cmake --build build --target ssh_cli_demo
```

Binary path:

- `build/libs/ssh_cli/ssh_cli_demo`

## Non-interactive command

```bash
./build/libs/ssh_cli/ssh_cli_demo \
  --host 127.0.0.1 \
  --port 22 \
  --user demo \
  --key /path/to/id_ed25519 \
  --cmd "echo HELLO"
```

## Interactive shell

```bash
./build/libs/ssh_cli/ssh_cli_demo \
  --host 127.0.0.1 \
  --port 22 \
  --user demo \
  --key /path/to/id_ed25519 \
  --interactive \
  --timeout-ms 300 \
  --poll-ms 15
```

## Optional strict host key checking

```bash
./build/libs/ssh_cli/ssh_cli_demo \
  --host 127.0.0.1 \
  --port 22 \
  --user demo \
  --key /path/to/id_ed25519 \
  --known-hosts /path/to/known_hosts \
  --strict-host-key \
  --cmd "echo SECURE"
```

## Interactive read tuning

- `--timeout-ms`: per-read wait timeout in milliseconds
- `--poll-ms`: polling interval used while waiting for interactive output

Use larger values for high-latency remote links.

## Integration tests

`ssh_cli` tests are grouped with ctest labels:

- unit: `ssh_cli_stub`, `ssh_cli_loopback`
- integration/smoke: `ssh_cli_process_probe`, `ssh_cli_process_local`

Run all `ssh_cli` tests:

```bash
ctest --test-dir build -L ssh_cli --output-on-failure
```

Enable external process probe test:

- `YUAN_RUN_SSH_CLI_PROCESS_PROBE=1`
- `YUAN_SSH_PROBE_HOST`
- `YUAN_SSH_PROBE_PORT`
- `YUAN_SSH_PROBE_USER`
- `YUAN_SSH_PROBE_KEY`

Enable local bootstrap integration test:

- `YUAN_RUN_SSH_CLI_LOCAL=1`

## VSCode Remote-SSH quick checklist (manual)

1. Ensure server supports publickey auth and interactive shell (`pty-req` + `shell`).
2. Verify command probe works:

```bash
ssh -T -oBatchMode=yes -i /path/to/id_ed25519 user@host "echo VSCODE_PROBE_OK"
```

3. Verify interactive shell works:

```bash
ssh -tt -i /path/to/id_ed25519 user@host
```

4. In VSCode, add the same host in SSH config and connect with Remote-SSH.
5. Confirm remote server bootstrap command can start and terminal resize behaves correctly.
