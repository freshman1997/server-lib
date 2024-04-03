#include "net/base/connection/connection.h"
#include "net/http/context.h"
#include "net/http/packet.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include "net/http/response_code.h"
#include "net/http/response_code_desc.h"

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
        std::string response;
        switch (errorCode) {
            case ResponseCode::bad_request: {
                errorCode = ResponseCode::bad_request;
                break;
            }
            case ResponseCode::not_found: {
                errorCode = ResponseCode::not_found;
                break;
            }
            default: {
                errorCode = ResponseCode::internal_server_error;
                break;
            }
        }

        std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">"+ responseCodeDescs[errorCode] +"</h1>";;
        response = "HTTP/1.1 " + std::to_string((int)errorCode) + " " + responseCodeDescs[errorCode] 
                    + "\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " 
                    + std::to_string(msg.size()) + "\r\n\r\n" + msg;

        auto buff = conn_->get_output_buff();
        buff->write_string(response);
        conn_->send();
        conn_->close();
    }

    HttpPacket * HttpSessionContext::get_packet()
    {
        return mode_ == Mode::server ? 
            static_cast<HttpPacket *>(request_) : static_cast<HttpPacket *>(response_);
    }
}