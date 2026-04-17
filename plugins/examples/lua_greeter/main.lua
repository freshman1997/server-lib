local plugin = {}
local ctx = nil
local tick_count = 0

function plugin.on_init(c)
    ctx = c

    ctx.logger:info("LuaGreeter initializing...")
    ctx.logger:info("app_name = " .. ctx.app_name)
    ctx.logger:info("plugin_name = " .. ctx.plugin_name)

    if ctx.config and ctx.config.greeting then
        ctx.logger:info("greeting from config: " .. ctx.config.greeting)
    end

    ctx.event_bus:subscribe("plugin.loaded", function(event)
        ctx.logger:info("LuaGreeter saw plugin loaded event")
    end)

    ctx.scheduler:schedule_interval(10000, function()
        tick_count = tick_count + 1
        ctx.logger:debug("LuaGreeter heartbeat #" .. tick_count)
    end, "LuaGreeter.heartbeat")

    if ctx.storage and ctx.storage:is_available() then
        ctx.storage:set("lua_greeter.status", "running")
        ctx.storage:set("lua_greeter.count", tostring(tick_count), 60000)
        local val = ctx.storage:get("lua_greeter.status")
        ctx.logger:info("storage read back: " .. tostring(val))
    end

    ctx.logger:info("LuaGreeter initialized successfully")
    return true
end

function plugin.on_enable()
    if ctx and ctx.logger then
        ctx.logger:info("LuaGreeter enabled")
    end
end

function plugin.on_disable()
    if ctx and ctx.logger then
        ctx.logger:info("LuaGreeter disabled")
    end
end

function plugin.on_health_check()
    return true
end

function plugin.on_release()
    if ctx and ctx.logger then
        ctx.logger:info("LuaGreeter releasing, total ticks = " .. tick_count)
    end
end

return plugin
