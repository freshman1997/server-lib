#ifndef __YUAN_NET_DATAGRAM_TRANSPORT_H__
#define __YUAN_NET_DATAGRAM_TRANSPORT_H__

namespace yuan::net
{

class InetAddress;
class UdpInstance;
enum class ConnectionState;

class DatagramTransport
{
public:
    virtual ~DatagramTransport() = default;

    virtual const InetAddress &peer_address() const = 0;
    virtual void attach_datagram_instance(UdpInstance *instance) = 0;
    virtual void set_datagram_state(ConnectionState state) = 0;
};

} // namespace yuan::net

#endif
