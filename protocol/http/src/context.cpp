#include "net/connection/connection.h"
#include "context.h"
#include "ops/option.h"
#include "packet.h"
#include "request.h"
#include "response.h"
#include "response_code.h"

namespace yuan::net::http
{
    namespace
    {
        template <typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

    HttpSessionContext::HttpSessionContext(Connection * conn)
        : mode_(Mode::server), has_parsed_(false), conn_(conn)
    {
        request_ = std::make_unique<HttpRequest>(this);
        response_ = std::make_unique<HttpResponse>(this);
    }

    HttpSessionContext::HttpSessionContext(const std::shared_ptr<Connection> &conn)
        : mode_(Mode::server), has_parsed_(false), conn_owner_(conn), conn_(ptr_of(conn))
    {
        request_ = std::make_unique<HttpRequest>(this);
        response_ = std::make_unique<HttpResponse>(this);
    }

    HttpSessionContext::~HttpSessionContext() = default;

    void HttpSessionContext::reset() const
    {
        request_->reset();
        response_->reset();
    }

    bool HttpSessionContext::parse()
    {
        if (!conn_)
            return false;

        if (!is_downloading() && !has_parsed_) {
            reset();
            has_parsed_ = true;
        }

        auto pkt = get_packet();
        pkt->parse(conn_->take_input_byte_buffer());
        return pkt->good();
    }

    bool HttpSessionContext::parse_from(const ::yuan::buffer::ByteBuffer & data)
    {
        if (!is_downloading() && !has_parsed_) {
            reset();
            has_parsed_ = true;
        }

        auto pkt = get_packet();
        pkt->parse(data);
        return pkt->good();
    }

    ::yuan::buffer::ByteBuffer HttpSessionContext::take_leftover_buffer()
    {
        return get_packet()->take_leftover_buffer();
    }

    bool HttpSessionContext::write() const
    {
        if (response_->is_uploading() && conn_->is_connected()) {
            ::yuan::buffer::ByteBuffer buffer(static_cast<std::size_t>(config::client_max_content_length + config::max_header_length));
            response_->write(buffer);
            if (!buffer.empty()) {
                conn_->write_and_flush(buffer);
            }
        }

        return true;
    }

    bool HttpSessionContext::is_completed()
    {
        if (!conn_)
            return false;

        if (get_packet()->is_ok()) {
            has_parsed_ = false;
            return true;
        }

        return false;
    }

    bool HttpSessionContext::has_error() const
    {
        return !get_packet()->good();
    }

    void HttpSessionContext::send() const
    {
        get_packet()->send();
    }

    ResponseCode HttpSessionContext::get_error_code() const
    {
        return get_packet()->get_error_code();
    }

    bool HttpSessionContext::try_parse_request_content() const
    {
        return get_packet()->parse_content();
    }

    void HttpSessionContext::process_error(const ResponseCode errorCode) const
    {
        if (mode_ == Mode::server) {
            response_->process_error(errorCode);
        }
    }

    HttpPacket *HttpSessionContext::get_packet() const
    {
        return mode_ == Mode::server ? static_cast<HttpPacket *>(ptr_of(request_)) : static_cast<HttpPacket *>(ptr_of(response_));
    }

    bool HttpSessionContext::is_downloading() const
    {
        const auto pkt = get_packet();
        return pkt->is_downloading() && pkt->is_task_prepared();
    }
}
