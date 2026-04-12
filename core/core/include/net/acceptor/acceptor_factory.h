#ifndef __YUAN_NET_ACCEPTOR_FACTORY_H__
#define __YUAN_NET_ACCEPTOR_FACTORY_H__

namespace yuan::timer
{
class TimerManager;
}

namespace yuan::net
{

class Socket;
class StreamAcceptor;
class DatagramAcceptor;

StreamAcceptor *create_stream_acceptor(Socket *socket);
DatagramAcceptor *create_datagram_acceptor(Socket *socket, yuan::timer::TimerManager *timer_manager);

} // namespace yuan::net

#endif
