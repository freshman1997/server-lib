#include "common/websocket_connection.h"
#include "entry/data_handler.h"
#include "entry/server.h"

#include "buffer/byte_buffer.h"
#include "net/runtime/network_runtime.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#endif

using namespace yuan::net::websocket;

namespace
{
    std::atomic_bool g_running{true};

    void signal_handler(int)
    {
        g_running.store(false, std::memory_order_release);
    }

    class AutobahnEchoHandler : public WebSocketDataHandler
    {
    public:
        void on_connected(WebSocketConnection *) override
        {
        }

        void on_data(WebSocketConnection *wsConn, const yuan::buffer::ByteBuffer &buff) override
        {
            auto type = WebSocketConnection::PacketType::text_;
            for (const auto &chunk : wsConn->input_chunks()) {
                if (chunk.body_.read_ptr() == buff.read_ptr() &&
                    chunk.body_.readable_bytes() == buff.readable_bytes()) {
                    if (chunk.head_.is_binary_frame()) {
                        type = WebSocketConnection::PacketType::binary_;
                    }
                    break;
                }
            }
            wsConn->send(buff, type);
        }

        void on_close(WebSocketConnection *) override
        {
        }
    };
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    int port = 12211;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "invalid port\n";
        return 1;
    }

    yuan::net::NetworkRuntime runtime;
    WebSocketServer server;
    AutobahnEchoHandler handler;
    server.set_data_handler(&handler);

    if (!server.init(port, runtime)) {
        std::cerr << "failed to init websocket autobahn server on port " << port << '\n';
        return 1;
    }

    server.serve();
    std::thread runtime_thread([&runtime]() {
        runtime.run();
    });

    std::cout << "websocket autobahn server listening on 127.0.0.1:" << port << '\n';
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    server.stop();
    runtime.stop();
    if (runtime_thread.joinable()) {
        runtime_thread.join();
    }
    return 0;
}
