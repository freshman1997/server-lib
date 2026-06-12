#ifndef YUAN_RPC_SERVER_H
#define YUAN_RPC_SERVER_H

#include "codec.h"
#include "local_bus.h"

#include <exception>
#include <utility>

namespace yuan::rpc
{
    class Server
    {
    public:
        bool register_handler(Route route, RequestHandler handler);

        template<typename Request, typename ResponseT, typename Handler>
        bool register_typed_handler(Route route, Handler handler)
        {
            return register_handler(
                std::move(route),
                [handler = std::move(handler)](const Message &message) mutable {
                    Response response;
                    response.serialization = CodecTraits<ResponseT>::serialization;
                    if (message.serialization != CodecTraits<Request>::serialization) {
                        response.status = RpcStatus::bad_request;
                        response.error = "rpc request serialization mismatch";
                        return response;
                    }

                    try {
                        response.payload = Codec<ResponseT>::encode(handler(Codec<Request>::decode(message.payload)));
                    } catch (const std::exception &e) {
                        response.status = RpcStatus::internal_error;
                        response.error = e.what();
                    } catch (...) {
                        response.status = RpcStatus::internal_error;
                        response.error = "unknown typed rpc handler error";
                    }
                    return response;
                });
        }

        bool unregister_handler(const Route &route);

        Response handle(const Message &message) const;

        [[nodiscard]] std::size_t handler_count() const;

    private:
        LocalBus bus_;
    };
}

#endif
