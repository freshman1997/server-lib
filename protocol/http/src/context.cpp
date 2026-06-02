#include "net/connection/connection.h"
#include "context.h"
#include "packet.h"
#include "request.h"
#include "response.h"
#include "response_code.h"
#include "base/owner_ptr.h"
#include "base/time.h"

namespace yuan::net::http
{
    HttpSessionContext::HttpSessionContext(Connection *conn)
        : mode_(Mode::server), has_parsed_(false), request_start_ms_(0), conn_(conn)
    {
        request_ = std::make_unique<HttpRequest>(this);
        response_ = std::make_unique<HttpResponse>(this);
    }

    HttpSessionContext::HttpSessionContext(const std::shared_ptr<Connection> &conn)
        : mode_(Mode::server), has_parsed_(false), request_start_ms_(0), conn_owner_(conn), conn_(yuan::base::owner_ptr(conn))
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
            request_start_ms_ = request_timing_enabled_ ? base::time::steady_now_ms() : 0;
        }

        auto pkt = get_packet();
        pkt->parse(conn_->take_input_byte_buffer());
        return pkt->good();
    }

    bool HttpSessionContext::parse_from(const ::yuan::buffer::ByteBuffer &data)
    {
        if (!is_downloading() && !has_parsed_) {
            reset();
            has_parsed_ = true;
            request_start_ms_ = request_timing_enabled_ ? base::time::steady_now_ms() : 0;
        }

        auto pkt = get_packet();
        pkt->parse(data);
        return pkt->good();
    }

    bool HttpSessionContext::parse_from(::yuan::buffer::ByteBuffer &&data)
    {
        if (!is_downloading() && !has_parsed_) {
            reset();
            has_parsed_ = true;
            request_start_ms_ = request_timing_enabled_ ? base::time::steady_now_ms() : 0;
        }

        auto pkt = get_packet();
        pkt->parse(std::move(data));
        return pkt->good();
    }

    ::yuan::buffer::ByteBuffer HttpSessionContext::take_leftover_buffer()
    {
        return get_packet()->take_leftover_buffer();
    }

    bool HttpSessionContext::write() const
    {
        if (response_->is_uploading() && conn_ && conn_->is_connected()) {
            if (auto *task = response_->get_task(); task && task->write_to_connection(conn_)) {
                return true;
            }

            ::yuan::buffer::ByteBuffer buffer(256 * 1024);
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
            auto *packet = get_packet();
            if (!packet->is_chunked() || packet->get_body_state() == BodyState::fully) {
                has_parsed_ = false;
            }
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
        return mode_ == Mode::server ? static_cast<HttpPacket *>(yuan::base::owner_ptr(request_)) : static_cast<HttpPacket *>(yuan::base::owner_ptr(response_));
    }

    bool HttpSessionContext::is_downloading() const
    {
        const auto pkt = get_packet();
        return pkt->is_downloading() && pkt->is_task_prepared();
    }

    uint64_t HttpSessionContext::request_elapsed_ms() const
    {
        if (request_start_ms_ == 0) {
            return 0;
        }
        const uint64_t now = base::time::steady_now_ms();
        return now > request_start_ms_ ? (now - request_start_ms_) : 0;
    }
}
