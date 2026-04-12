#ifndef __YUAN_NET_STREAM_TRANSPORT_H__
#define __YUAN_NET_STREAM_TRANSPORT_H__

namespace yuan::net
{

class Channel;

class StreamTransport
{
public:
    virtual ~StreamTransport() = default;

    virtual Channel *stream_channel() = 0;
};

} // namespace yuan::net

#endif
