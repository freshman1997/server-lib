#include "net/connection/connection_factory.h"

#include "net/acceptor/udp/adapter.h"
#include "net/connection/tcp_connection.h"
#include "net/connection/udp_connection.h"

namespace yuan::net
{

Connection *create_stream_connection(Socket *socket)
{
    return new TcpConnection(socket);
}

Connection *create_stream_connection(const std::string &ip, int port, int fd)
{
    return new TcpConnection(ip, port, fd);
}

Connection *create_datagram_connection(const InetAddress &address)
{
    return new UdpConnection(address);
}

Connection *create_datagram_connection(const InetAddress &address, UdpAdapter *adapter)
{
    return new UdpConnection(address, adapter);
}

} // namespace yuan::net
