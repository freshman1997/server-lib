# LuaLineEchoProtocol

Minimal Lua protocol-service plugin using a custom stream handler.

The manifest declares `transport = "tcp"`, `framing = "line"`, and
`handler = "main.on_connection"`. The Lua handler reads one line from the
connection (`conn:read_line(30000)`), echoes it with a trailing newline, and
flushes.

`HostStreamConnection` methods available in Lua:

- `id()`
- `peer_address()`
- `local_address()`
- `read(timeout_ms)`
- `read_line(timeout_ms)`
- `write(data)`
- `flush()`
- `close()`
- `is_open()`

Example handler:

```lua
function plugin.on_connection(conn, data)
  local line = conn:read_line(30000)
  if line == nil then
    line = data
  end
  if line == nil then
    return false
  end

  conn:write(line)
  conn:write("\n")
  conn:flush()
  return true
end
```
