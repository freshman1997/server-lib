#include "net/base/connection/connection.h"
#include "net/http/request_context.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include "net/http/response_code.h"
#include "net/http/response_code_desc.h"

namespace net::http 
{
    HttpRequestContext::HttpRequestContext(Connection *conn) : conn_(conn), has_parsed_(false)
    {
        request_ = new HttpRequest(this);
        response_ = new HttpResponse(this);
    }

    HttpRequestContext::~HttpRequestContext()
    {
        delete request_;
        delete response_;
    }

    void HttpRequestContext::reset()
    {
        request_->reset();
        response_->reset();
    }

    bool HttpRequestContext::parse()
    {
        if (!conn_) {
            return false;
        }

        if (!has_parsed_) {
            reset();
        }

        has_parsed_ = true;
        return  request_->parse(*conn_->get_input_buff());
    }

    bool HttpRequestContext::is_completed()
    {
        if (!conn_ || !request_) {
            return false;
        }

        if (request_->is_ok()) {
            has_parsed_ = false;
            return true;
        }
        
        return false;
    }

    bool HttpRequestContext::has_error()
    {
        return !request_->good();
    }

    void HttpRequestContext::send()
    {
        response_->send();
    }

    ResponseCode HttpRequestContext::get_error_code() const 
    {
        return request_->get_error_code();
    }

    bool HttpRequestContext::try_parse_request_content()
    {
        return request_->parse_content();
    }
    
    void HttpRequestContext::process_error(ResponseCode errorCode)
    {
        std::string response;
        switch (errorCode) {
            case ResponseCode::bad_request: {
                std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">"+ responseCodeDescs[errorCode] +"</h1>";
                response = "HTTP/1.1 "+ responseCodeDescs[errorCode] 
                    + "\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " 
                    + std::to_string(msg.size()) + "\r\n\r\n" + msg;
                break;
            }
            case ResponseCode::not_found: {
                std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">"+ responseCodeDescs[errorCode] +"</h1>";
                response = "HTTP/1.1 " + responseCodeDescs[errorCode] 
                    + "\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " 
                    + std::to_string(msg.size()) + "\r\n\r\n" + msg;
                break;
            }
            default: {
                std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">"+ responseCodeDescs[ResponseCode::internal_server_error] +"</h1>";
                response = "HTTP/1.1 " + responseCodeDescs[ResponseCode::internal_server_error] 
                    + "\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " 
                    + std::to_string(msg.size()) + "\r\n\r\n" + msg;
                break;
            }
        }

        auto buff = conn_->get_output_buff();
        buff->write_string(response);
        conn_->send();
        conn_->close();
    }
}