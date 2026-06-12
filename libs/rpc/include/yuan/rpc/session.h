#ifndef YUAN_RPC_SESSION_H
#define YUAN_RPC_SESSION_H

#include "channel.h"
#include "frame_stream.h"
#include "server.h"
#include "transport.h"

#include <memory>
#include <mutex>
#include <utility>

namespace yuan::rpc
{
    class RpcSession final : public IChannel, public std::enable_shared_from_this<RpcSession>
    {
    public:
        explicit RpcSession(std::shared_ptr<IFrameTransport> transport,
                            Server *server = nullptr,
                            wire::EncodeOptions encode_options = {},
                            wire::DecodeOptions decode_options = {});

        void start();

        bool send(Message message, ResponseHandler on_response, CallOptions options = {}) override;

        bool push(Message message) override;

        void poll_timeouts() override;

        void set_server(Server *server);

        void receive_bytes(const std::uint8_t *data, std::size_t size);

        [[nodiscard]] const TransportStats &stats() const;

        [[nodiscard]] std::size_t pending_size() const;

    private:
        void handle_frame(wire::DecodedFrame frame);

        void cancel_pending(std::string error);

        std::shared_ptr<IFrameTransport> transport_;
        Server *server_ = nullptr;
        wire::EncodeOptions encode_options_;
        wire::FrameStreamDecoder decoder_;
        PendingCallRegistry pending_;
        TransportStats stats_;
    };
}

#endif
