#include "buffer/linked_buffer.h"
#include "net/connection/connection.h"
#include "context.h"
#include "ops/option.h"
#include "packet.h"
#include "request.h"
#include "response.h"
#include "response_code.h"

namespace yuan::net::http 
{
    HttpSessionContext::HttpSessionContext(Connection *conn) : mode_(Mode::server), has_parsed_(false), conn_(conn)
    {
        request_ = new HttpRequest(this);
        response_ = new HttpResponse(this);
    }

    HttpSessionContext::~HttpSessionContext()
    {
        delete request_;
        delete response_;
    }

    void HttpSessionContext::reset() const
    {
        request_->reset();
        response_->reset();
    }

    bool HttpSessionContext::parse()
    {
        if (!conn_) {
            return false;
        }

        if (!is_donwloading() && !has_parsed_) {
            reset();
            has_parsed_ = true;
        }

        auto pkt = get_packet();
        pkt->parse(*conn_->get_input_buff());
        return pkt->good();
    }

    bool HttpSessionContext::write()
    {
        if (response_->is_uploading() && conn_->is_connected())
        {
            auto buff = response_->get_buff(true);
            buff->reset();
            buff->resize(static_cast<size_t>(config::client_max_content_length + config::max_header_length));
            response_->write(*buff);
            conn_->write_and_flush(buff);
        }

        return true;
    }

    bool HttpSessionContext::is_completed()
    {
        if (!conn_) {
            return false;
        }

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
    
    HttpPacket * HttpSessionContext::get_packet() const
    {
        return mode_ == Mode::server ? 
            static_cast<HttpPacket *>(request_) : static_cast<HttpPacket *>(response_);
    }

    bool HttpSessionContext::is_donwloading() const
    {
        auto pkt = get_packet();
        return pkt->is_downloading() && pkt->is_task_prepared();
    }
}