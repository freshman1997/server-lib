# TsLineEchoProtocol

Minimal TypeScript protocol-service plugin using a custom stream handler.

The manifest declares `transport = "tcp"`, `framing = "line"`, and
`handler = "main.onConnection"`. The TypeScript handler reads one line from
the connection (`conn.readLine(30000)`), echoes it with a trailing newline,
and flushes.

`HostStreamConnection` methods available in TypeScript:

- `id()`
- `peerAddress()`
- `localAddress()`
- `read(timeoutMs)`
- `readLine(timeoutMs)`
- `write(data)`
- `flush()`
- `close()`
- `isOpen()`
