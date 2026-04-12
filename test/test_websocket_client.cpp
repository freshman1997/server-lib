#include "websocket.h"
#include <iostream>

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
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    TestClient tc;
    net::websocket::WebSocketClient client;
    if (!client.init() || !client.connect({"localhost", 12211})) {
        return -1;
    }

    client.set_data_handler(&tc);
    client.run();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
