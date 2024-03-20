#include "net/http/response.h"
#include "buffer/buffer.h"
#include "net/http/request_context.h"
#include "net/connection/connection.h"
#include "net/http/response_code_desc.h"
#include <cstdint>
#include <iostream>

namespace net::http
{
    HttpResponse::HttpResponse(HttpRequestContext *context) : context_(context), buffer_(std::make_shared<Buffer>())
    {
        
    }

    void HttpResponse::append_body(const char *data)
    {
        buffer_->write_string(data);
    }

    void HttpResponse::append_body(const std::string &data)
    {
        buffer_->write_string(data);
    }

    void HttpResponse::reset()
    {
        respCode_ = response_code::ResponseCode::bad_request;
        version_ = HttpVersion::v_1_1;
        headers_.clear();
        buffer_->reset();
    }

    void HttpResponse::send()
    {
        bool res = pack_response();
        if (res) {
            context_->get_connection()->send(buffer_);
        } else {
            context_->get_connection()->close();
        }
    }

    void HttpResponse::pack_error_reponse()
    {
        auto outputBuffer = context_->get_connection()->get_output_buff();
        std::string header("HTTP/1.1");
        header.append(" ");
        header.append(std::to_string((uint32_t)response_code::ResponseCode::internal_server_error));
        header.append(" ");
        header.append(responseCodeDescs[response_code::ResponseCode::internal_server_error]);
        header.append("\r\nContent-Type: text/html\r\n");
        header.append("Content-Length: 93");
        header.append("\r\n\r\n");
        header.append("<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">internal server error</h1>");
        outputBuffer->write_string(header);
    }

    bool HttpResponse::pack_response()
    {
        auto descIt = responseCodeDescs.find(respCode_);
        if (descIt == responseCodeDescs.end() || respCode_ == response_code::ResponseCode::internal_server_error) {
            pack_error_reponse();
            return false;
        }
        
        auto outputBuffer = context_->get_connection()->get_output_buff();
        std::string header("HTTP/1.1");
        header.append(" ");
        header.append(std::to_string((uint32_t)respCode_));
        header.append(" ");
        header.append(descIt->second).append("\r\n");

        for (const auto &item : headers_) {
            header.append(item.first).append(": ").append(item.second).append("\r\n");
        }

        header.append("\r\n");
        outputBuffer->write_string(header);

        return true;
    }
}