#include "base/time.h"
#include "base/utils/base64.h"
#include "buffer/pool.h"
#include "websocket.h"
#include "buffer/buffer.h"
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#else
#include <signal.h>
#endif

class TestServer : public net::websocket::WebSocketDataHandler
{
public:
    virtual void on_connected(net::websocket::WebSocketConnection *wsConn)
    {

    }

    virtual void on_data(net::websocket::WebSocketConnection *wsConn, const Buffer *buff)
    {
        std::ofstream file("data.txt");
        if (file.good()) {
            file.write(buff->peek(), buff->readable_bytes());
            file.close();
        }

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
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    net::websocket::WebSocketServer wsSvr;
    if (!wsSvr.init(12211)) {
        return -1;
    }

    TestServer testSvr;
    wsSvr.set_data_handler(&testSvr);

    wsSvr.serve();
    
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}