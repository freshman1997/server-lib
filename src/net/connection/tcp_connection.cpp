#include "net/connection/tcp_connection.h"

namespace net::conn 
{
    TcpConnection::TcpConnection(net::InetAddress *addr) : addr_(addr) {}
    
}