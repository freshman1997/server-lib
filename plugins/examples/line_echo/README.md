# LineEchoProtocol

Minimal C++ protocol-service plugin using the stream handler registry.

The manifest declares `transport = "tcp"`, `framing = "line"`, and
`handler = "line_echo.on_connection"`. The plugin registers that handler in
`register_protocol_handlers()` and echoes each received line with a trailing
newline.
