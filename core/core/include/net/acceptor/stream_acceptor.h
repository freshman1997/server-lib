#ifndef __YUAN_NET_ACCEPTOR_STREAM_ACCEPTOR_H__
#define __YUAN_NET_ACCEPTOR_STREAM_ACCEPTOR_H__

#include "acceptor.h"
#include "stream_listener.h"

namespace yuan::net
{

class StreamAcceptor : public Acceptor, public StreamListener
{
public:
    ~StreamAcceptor() override = default;
};

} // namespace yuan::net

#endif
