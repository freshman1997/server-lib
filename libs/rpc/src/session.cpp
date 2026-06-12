#include "yuan/rpc/session.h"

#include <utility>

namespace yuan::rpc
{
    RpcSession::RpcSession(std::shared_ptr<IFrameTransport> transport,
                           Server *server,
                           wire::EncodeOptions encode_options,
                           wire::DecodeOptions decode_options)
        : transport_(std::move(transport)),
          server_(server),
          encode_options_(std::move(encode_options)),
          decoder_(std::move(decode_options))
    {
    }

    void RpcSession::start()
    {
        if (!transport_) {
            return;
        }
        auto weak = weak_from_this();
        transport_->set_receive_callback([weak](const std::uint8_t *data, std::size_t size) {
            if (auto self = weak.lock()) {
                self->receive_bytes(data, size);
            }
        });
        transport_->set_close_callback([weak] {
            if (auto self = weak.lock()) {
                self->cancel_pending("transport closed");
            }
        });
    }

    bool RpcSession::send(Message message, ResponseHandler on_response, CallOptions options)
    {
        if (!transport_ || !transport_->is_open()) {
            return false;
        }

        const RequestId id = pending_.create(std::move(on_response), options.timeout, options.metadata, options.continuation_id());
        if (id == 0) {
            return false;
        }

        message.kind = MessageKind::request;
        message.request_id = id;
        if (message.coroutine_id == 0) {
            message.coroutine_id = options.coroutine_id;
        }
        if (message.serialization == Serialization::raw) {
            message.serialization = options.serialization;
        }
        if (message.compression == Compression::none) {
            message.compression = options.compression;
        }
        if (message.encryption == Encryption::none) {
            message.encryption = options.encryption;
            message.key_id = options.key_id;
            message.nonce = options.nonce;
        }

        Bytes frame;
        if (!wire::encode_message(message, frame, encode_options_) || !transport_->send_frame(frame)) {
            (void)pending_.cancel(id, "send failed");
            stats_.send_errors++;
            return false;
        }
        stats_.frames_sent++;
        stats_.bytes_sent += frame.size();
        return true;
    }

    bool RpcSession::push(Message message)
    {
        if (!transport_ || !transport_->is_open()) {
            return false;
        }

        message.kind = MessageKind::push;
        Bytes frame;
        if (!wire::encode_message(message, frame, encode_options_) || !transport_->send_frame(frame)) {
            stats_.send_errors++;
            return false;
        }
        stats_.frames_sent++;
        stats_.bytes_sent += frame.size();
        return true;
    }

    void RpcSession::poll_timeouts()
    {
        (void)pending_.expire();
    }

    void RpcSession::set_server(Server *server)
    {
        server_ = server;
    }

    void RpcSession::receive_bytes(const std::uint8_t *data, std::size_t size)
    {
        decoder_.append(data, size);
        for (;;) {
            auto result = decoder_.next();
            if (result.ok) {
                stats_.frames_received++;
                stats_.bytes_received += result.consumed;
                handle_frame(std::move(result.frame));
                continue;
            }
            if (result.error != wire::DecodeError::need_more) {
                stats_.decode_errors++;
                decoder_.clear();
            }
            break;
        }
    }

    const TransportStats &RpcSession::stats() const
    {
        return stats_;
    }

    std::size_t RpcSession::pending_size() const
    {
        return pending_.size();
    }

    void RpcSession::handle_frame(wire::DecodedFrame frame)
    {
        if (frame.header.kind == MessageKind::response) {
            (void)pending_.complete(wire::to_response(std::move(frame)));
            return;
        }

        if (!server_) {
            return;
        }

        auto message = wire::to_message(std::move(frame));
        auto response = server_->handle(message);
        if (message.kind == MessageKind::push) {
            return;
        }

        response.request_id = message.request_id;
        response.coroutine_id = message.coroutine_id;
        response.serialization = response.serialization == Serialization::raw ? message.serialization : response.serialization;
        response.compression = response.compression == Compression::none ? message.compression : response.compression;
        Bytes out;
        if (wire::encode_response(response, out, encode_options_) && transport_ && transport_->is_open() && transport_->send_frame(out)) {
            stats_.frames_sent++;
            stats_.bytes_sent += out.size();
        } else {
            stats_.send_errors++;
        }
    }

    void RpcSession::cancel_pending(std::string error)
    {
        (void)error;
        // PendingCallRegistry currently cancels by id; timed cleanup remains available through poll_timeouts().
    }
}
