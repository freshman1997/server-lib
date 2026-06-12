#include "yuan/rpc/client.h"

#include <utility>

namespace yuan::rpc
{
    Client::Client(IChannel &channel)
        : channel_(channel)
    {
    }

    bool Client::call(Route route, Bytes payload, ResponseHandler on_response, CallOptions options)
    {
        Message message;
        message.kind = MessageKind::request;
        message.route = std::move(route);
        message.coroutine_id = options.coroutine_id;
        message.serialization = options.serialization;
        message.compression = options.compression;
        message.encryption = options.encryption;
        message.key_id = options.key_id;
        message.nonce = options.nonce;
        message.metadata = options.metadata;
        message.payload = std::move(payload);
        return channel_.send(std::move(message), std::move(on_response), std::move(options));
    }

    bool Client::push(Route route, Bytes payload, Metadata metadata)
    {
        Message message;
        message.kind = MessageKind::push;
        message.route = std::move(route);
        message.metadata = std::move(metadata);
        message.payload = std::move(payload);
        return channel_.push(std::move(message));
    }
}
