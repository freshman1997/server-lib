#ifndef __YUAN_NET_CONNECTION_FACTORY_H__
#define __YUAN_NET_CONNECTION_FACTORY_H__

#include <memory>
#include <string>

#include "connection.h"

namespace yuan::net
{

class InetAddress;
class Socket;
class UdpAdapter;

ConnectionPtr create_stream_connection(Socket *socket);
ConnectionPtr create_stream_connection(const std::string &ip, int port, int fd);
ConnectionPtr create_datagram_connection(const InetAddress &address);
ConnectionPtr create_datagram_connection(const InetAddress &address, UdpAdapter *adapter);

} // namespace yuan::net

#endif
