var tickCount = 0;

function on_init(host) {
    host.logger.info("TsGreeter initializing...");
    host.logger.info("app_name = " + host.appName);
    host.logger.info("plugin_name = " + host.pluginName);

    if (host.config && host.config.greeting) {
        host.logger.info("greeting from config: " + host.config.greeting);
    }

    host.eventBus.subscribe("plugin.loaded", function(event) {
        host.logger.info("TsGreeter saw plugin loaded event: " + event.name);
    });

    host.scheduler.schedule_interval(10000, function() {
        tickCount++;
        host.logger.debug("TsGreeter heartbeat #" + tickCount);
    }, "TsGreeter.heartbeat");

    host.scheduler.schedule_after(3000, function() {
        host.logger.info("TsGreeter one-shot delayed task fired!");
    }, "TsGreeter.delayed");

    if (host.storage && host.storage.is_available()) {
        host.storage.set("ts_greeter.status", "running");
        host.storage.set("ts_greeter.count", String(tickCount));
        var val = host.storage.get("ts_greeter.status");
        host.logger.info("storage read back: " + val);
    }

    host.eventBus.publish("ts_greeter.ready", { from: host.pluginName });

    host.logger.info("TsGreeter initialized successfully");
    return true;
}

function on_enable() {
}

function on_disable() {
}

function on_health_check() {
    return true;
}

function on_release() {
}
