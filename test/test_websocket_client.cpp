#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "websocket.h"
#include <iostream>

class TestClient : public net::websocket::WebSocketDataHandler
{
public:
    virtual void on_connected(net::websocket::WebSocketConnection *wsConn)
    {
        Buffer *buf = BufferedPool::get_instance()->allocate();
        buf->write_string("hello world!!!");
        wsConn->send(buf);
    }

    virtual void on_data(net::websocket::WebSocketConnection *wsConn, const Buffer *buff)
    {
        std::string str(buff->peek(), buff->peek_end());
        std::cout << "recv: " << str << '\n';
    }

    virtual void on_close(net::websocket::WebSocketConnection *wsConn)
    {

    }
};

int main()
{
    TestClient tc;
    net::websocket::WebSocketClient client;
    if (!client.create({"192.168.96.128", 12211})) {
        return -1;
    }

    client.set_data_handler(&tc);
    client.run();

    return 0;
}