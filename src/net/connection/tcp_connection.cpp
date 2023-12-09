#include "net/connection/tcp_connection.h"

namespace net
{
    TcpConnection::TcpConnection(net::InetAddress *addr) : addr_(addr) {}
    
}