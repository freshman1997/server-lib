#include "net/connection/connection_factory.h"

#include "net/acceptor/udp/adapter.h"
#include "net/connection/tcp_connection.h"
#include "net/connection/udp_connection.h"

namespace yuan::net
{

ConnectionPtr create_stream_connection(Socket *socket)
{
    return std::make_shared<TcpConnection>(socket);
}

ConnectionPtr create_stream_connection(const std::string &ip, int port, int fd)
{
    return std::make_shared<TcpConnection>(ip, port, fd);
}

ConnectionPtr create_datagram_connection(const InetAddress &address)
{
    return std::make_shared<UdpConnection>(address);
}

ConnectionPtr create_datagram_connection(const InetAddress &address, UdpAdapter *adapter)
{
    return std::make_shared<UdpConnection>(address, adapter);
}

} // namespace yuan::net
