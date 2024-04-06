#include "net/base/connection/connection.h"
#include "net/http/context.h"
#include "net/http/packet.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include "net/http/response_code.h"

namespace net::http 
{
    HttpSessionContext::HttpSessionContext(net::Connection *conn) : conn_(conn), has_parsed_(false), mode_(Mode::server)
    {
        request_ = new HttpRequest(this);
        response_ = new HttpResponse(this);
    }

    HttpSessionContext::~HttpSessionContext()
    {
        delete request_;
        delete response_;
    }

    void HttpSessionContext::reset()
    {
        request_->reset();
        response_->reset();
    }

    bool HttpSessionContext::parse()
    {
        if (!conn_) {
            return false;
        }

        if (!has_parsed_) {
            reset();
        }

        has_parsed_ = true;

        return get_packet()->parse(*conn_->get_input_buff());
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

    bool HttpSessionContext::has_error()
    {
        return !get_packet()->good();
    }

    void HttpSessionContext::send()
    {
        get_packet()->send();
    }

    ResponseCode HttpSessionContext::get_error_code() 
    {
        return get_packet()->get_error_code();
    }

    bool HttpSessionContext::try_parse_request_content()
    {
        return get_packet()->parse_content();
    }
    
    void HttpSessionContext::process_error(ResponseCode errorCode)
    {
        if (mode_ == Mode::server) {
            response_->process_error(errorCode);
        }
    }
    
    HttpPacket * HttpSessionContext::get_packet()
    {
        return mode_ == Mode::server ? 
            static_cast<HttpPacket *>(request_) : static_cast<HttpPacket *>(response_);
    }
}