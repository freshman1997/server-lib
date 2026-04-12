#ifndef __YUAN_NET_ACCEPTOR_DATAGRAM_ACCEPTOR_H__
#define __YUAN_NET_ACCEPTOR_DATAGRAM_ACCEPTOR_H__

#include "acceptor.h"
#include "datagram_endpoint.h"

namespace yuan::net
{
class UdpInstance;

class DatagramAcceptor : public Acceptor, public DatagramEndpoint
{
public:
    ~DatagramAcceptor() override = default;

    virtual UdpInstance *get_udp_instance() = 0;
};

} // namespace yuan::net

#endif
