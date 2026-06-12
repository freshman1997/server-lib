#ifndef YUAN_RPC_CLIENT_H
#define YUAN_RPC_CLIENT_H

#include "channel.h"
#include "codec.h"

#include <functional>
#include <utility>

namespace yuan::rpc
{
    class Client
    {
    public:
        explicit Client(IChannel &channel);

        bool call(Route route, Bytes payload, ResponseHandler on_response, CallOptions options = {});

        template<typename Request, typename ResponseT>
        bool call_typed(Route route,
                        const Request &request,
                        std::function<void(RpcStatus, ResponseT, std::string)> on_response,
                        CallOptions options = {})
        {
            if (!on_response) {
                return false;
            }
            options.serialization = CodecTraits<Request>::serialization;
            return call(
                std::move(route),
                Codec<Request>::encode(request),
                [callback = std::move(on_response)](Response response) mutable {
                    if (response.status != RpcStatus::ok) {
                        callback(response.status, ResponseT{}, std::move(response.error));
                        return;
                    }
                    callback(response.status, Codec<ResponseT>::decode(response.payload), {});
                },
                std::move(options));
        }

        bool push(Route route, Bytes payload, Metadata metadata = {});

        template<typename Event>
        bool push_typed(Route route, const Event &event, Metadata metadata = {})
        {
            Message message;
            message.kind = MessageKind::push;
            message.route = std::move(route);
            message.serialization = CodecTraits<Event>::serialization;
            message.metadata = std::move(metadata);
            message.payload = Codec<Event>::encode(event);
            return channel_.push(std::move(message));
        }

    private:
        IChannel &channel_;
    };
}

#endif
