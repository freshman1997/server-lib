#include "net/http/request_context.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include "net/connection/connection.h"

namespace net::http 
{
    HttpRequestContext::HttpRequestContext(Connection *conn) : conn_(conn)
    {
        request_ = new HttpRequest(this);
        response_ = new HttpResponse(this);
        init = true;
    }

    HttpRequestContext::~HttpRequestContext()
    {
        delete request_;
        delete response_;
    }

    void HttpRequestContext::pre_request()
    {
        if (!init) {
            request_->reset();
            response_->reset();
        }
    }

    bool HttpRequestContext::parse()
    {
        if (!conn_) {
            return false;
        }

        return request_->parse(*conn_->get_input_buff());
    }

    bool HttpRequestContext::is_completed()
    {
        if (!conn_ || !request_) {
            return false;
        }

        return request_->is_ok();
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

    void HttpRequestContext::process_error(Connection *conn, ResponseCode errorCode)
    {
        switch (errorCode) {
            case ResponseCode::bad_request: {
                std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">Bad Request</h1>";
                std::string repsonse = "HTTP/1.1 403 Bad Request\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
                auto buff = conn->get_output_buff();
                buff->write_string(repsonse);
                conn->send();
                conn->close();
                break;
            }
            case ResponseCode::not_found: {
                std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">resource not found</h1>";
                std::string repsonse = "HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
                auto buff = conn->get_output_buff();
                buff->write_string(repsonse);
                conn->send();
                conn->close();
                break;
            }
            default: {
                std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">Internal Server Error</h1>";
                std::string repsonse = "HTTP/1.1 500\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
                auto buff = conn->get_output_buff();
                buff->write_string(repsonse);
                conn->send();
                conn->close();
            }
        }
    }
}