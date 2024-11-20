#include "buffer/pool.h"
#include "websocket.h"
#include "buffer/buffer.h"
#include <iostream>

class TestServer : public net::websocket::WebSocketDataHandler
{
public:
    virtual void on_connected(net::websocket::WebSocketConnection *wsConn)
    {

    }

    virtual void on_data(net::websocket::WebSocketConnection *wsConn, const Buffer *buff)
    {
        std::string str(buff->peek(), buff->peek_end());
        std::cout << "recv: " << str << '\n';
        Buffer *data = BufferedPool::get_instance()->allocate(buff->readable_bytes());
        data->append_buffer(*buff);
        wsConn->send(data);
    }

    virtual void on_close(net::websocket::WebSocketConnection *wsConn)
    {

    }
};

int main()
{
    net::websocket::WebSocketServer wsSvr;
    if (!wsSvr.init()) {
        return -1;
    }

    TestServer testSvr;
    wsSvr.set_data_handler(&testSvr);

    wsSvr.serve();

    return 0;
}