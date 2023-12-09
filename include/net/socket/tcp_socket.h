#ifndef __TCP_SOCKET_H__
#define __TCP_SOCKET_H__

class InetAddress;

namespace net
{
    class TcpSocket
    {
    public:
        int bind(InetAddress *addr);

        int connect(InetAddress *addr, int timeout);

        /**
         * 主动关闭，未发送完的数据会继续发送，，发完数据后销毁
         */
        void close();

        /**
         * 未发完的数据丢弃，立即销毁
         */
        void shutdown();

        InetAddress * get_local_address();

        InetAddress * get_remote_address();
        
    };
}

#endif
