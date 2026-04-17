#ifndef __NET_ACCEPTOR_STREAM_LISTENER_H__
#define __NET_ACCEPTOR_STREAM_LISTENER_H__

namespace yuan::net
{
    class Channel;

    class StreamListener
    {
    public:
        virtual ~StreamListener() = default;

        virtual Channel *listener_channel() const = 0;
    };
}

#endif
