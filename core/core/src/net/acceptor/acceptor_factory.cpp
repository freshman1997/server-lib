#include "net/acceptor/acceptor_factory.h"

#include "net/acceptor/tcp_acceptor.h"
#include "net/acceptor/udp_acceptor.h"

namespace yuan::net
{

StreamAcceptor *create_stream_acceptor(Socket *socket)
{
    return new TcpAcceptor(socket);
}

DatagramAcceptor *create_datagram_acceptor(Socket *socket, yuan::timer::TimerManager *timer_manager)
{
    return new UdpAcceptor(socket, timer_manager);
}

} // namespace yuan::net
