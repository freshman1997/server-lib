#include "websocket.h"
#include <iostream>
#include "common/winsock_guard.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <signal.h>
#endif

using namespace yuan;

class TestClient : public net::websocket::WebSocketDataHandler
{
public:
    virtual void on_connected(net::websocket::WebSocketConnection *wsConn)
    {
        ::yuan::buffer::ByteBuffer buf(std::string_view("hello world!!!"));
        wsConn->send(buf);
    }

    virtual void on_data(net::websocket::WebSocketConnection *wsConn, const ::yuan::buffer::ByteBuffer &buff)
    {
        std::string str(buff.read_ptr(), buff.readable_bytes());
        std::cout << "recv: " << str << '\n';
    }

    virtual void on_close(net::websocket::WebSocketConnection *wsConn)
    {
    }
};

int main()
{
    const test::common::WinsockGuard winsock;
    if (!winsock.ok()) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    TestClient tc;
    net::websocket::WebSocketClient client;
    if (!client.init()) {
        return -1;
    }

    client.set_data_handler(&tc);
    if (!client.connect({ "localhost", 12211 })) {
        return -1;
    }
    client.run();

    return 0;
}
