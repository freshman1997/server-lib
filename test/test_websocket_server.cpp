#include "websocket.h"

class TestServer : public net::websocket::WebSocketDataHandler
{
public:
    virtual void on_data(net::websocket::WebSocketConnection *wsConn, const Buffer *buff)
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