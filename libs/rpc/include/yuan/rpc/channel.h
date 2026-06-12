#ifndef YUAN_RPC_CHANNEL_H
#define YUAN_RPC_CHANNEL_H

#include "pending_call_registry.h"

namespace yuan::rpc
{
    class IChannel
    {
    public:
        virtual ~IChannel() = default;

        virtual bool send(Message message, ResponseHandler on_response, CallOptions options = {}) = 0;
        virtual bool push(Message message) = 0;
        virtual void poll_timeouts() = 0;
    };
}

#endif
