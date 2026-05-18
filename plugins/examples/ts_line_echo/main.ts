function on_init(host) {
    if (host && host.logger) {
        host.logger.info("TsLineEchoProtocol initialized");
    }
    return true;
}

function onConnection(conn, data) {
    var line = conn.readLine(30000);
    if (line == null) {
        line = data;
    }
    if (line == null) {
        return false;
    }

    conn.write(line);
    conn.write("\n");
    conn.flush();
    return true;
}

function on_enable() {
}

function on_disable() {
    return;
}

function on_health_check() {
    return true;
}

function on_release() {
    return;
}
