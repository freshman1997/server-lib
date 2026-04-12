#ifndef __YUAN_NET_CONNECTION_FACTORY_H__
#define __YUAN_NET_CONNECTION_FACTORY_H__

#include <string>

namespace yuan::net
{

class Connection;
class InetAddress;
class Socket;
class UdpAdapter;

Connection *create_stream_connection(Socket *socket);
Connection *create_stream_connection(const std::string &ip, int port, int fd);
Connection *create_datagram_connection(const InetAddress &address);
Connection *create_datagram_connection(const InetAddress &address, UdpAdapter *adapter);

} // namespace yuan::net

#endif
