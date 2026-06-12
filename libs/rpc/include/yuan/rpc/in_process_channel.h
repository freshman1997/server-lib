#ifndef YUAN_RPC_IN_PROCESS_CHANNEL_H
#define YUAN_RPC_IN_PROCESS_CHANNEL_H

#include "channel.h"
#include "server.h"

#include <utility>

namespace yuan::rpc
{
    class InProcessChannel final : public IChannel
    {
    public:
        explicit InProcessChannel(Server &server)
            : server_(server)
        {
        }

        bool send(Message message, ResponseHandler on_response, CallOptions options = {}) override
        {
            const RequestId id = pending_.create(std::move(on_response), options.timeout, options.metadata, options.continuation_id());
            if (id == 0) {
                return false;
            }
            message.request_id = id;
            auto response = server_.handle(message);
            response.request_id = id;
            response.coroutine_id = message.coroutine_id;
            return pending_.complete(std::move(response));
        }

        bool push(Message message) override
        {
            message.kind = MessageKind::push;
            (void)server_.handle(message);
            return true;
        }

        void poll_timeouts() override
        {
            (void)pending_.expire();
        }

        [[nodiscard]] std::size_t pending_size() const
        {
            return pending_.size();
        }

    private:
        Server &server_;
        PendingCallRegistry pending_;
    };
}

#endif
