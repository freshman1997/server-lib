local plugin = {}

function plugin.on_init(ctx)
  if ctx and ctx.logger then
    ctx.logger:info("LuaLineEchoProtocol initialized")
  end
  return true
end

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

function plugin.on_enable()
end

function plugin.on_disable()
end

function plugin.on_health_check()
  return true
end

function plugin.on_release()
end

return plugin
