#include "net/http/response.h"
#include "buffer/buffer.h"
#include "net/http/request_context.h"
#include "net/connection/connection.h"
#include "net/http/response_code_desc.h"
#include <cstdint>

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

    void HttpResponse::send()
    {
        bool res = pack_response();
        context_->get_connection()->send();
        if (res) {
            context_->get_connection()->send(buffer_);
        } else {
            context_->get_connection()->close();
        }
    }

    void HttpResponse::pack_error_reponse()
    {
        auto outputBuffer = context_->get_connection()->get_output_buff();
        outputBuffer->reset();
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
        outputBuffer->reset();

        std::string header("HTTP/1.1");
        header.append(" ");
        header.append(std::to_string((uint32_t)respCode_));
        header.append(" ");
        header.append(descIt->second);
        outputBuffer->write_string(header);
        outputBuffer->write_string("\r\n");

        for (const auto &item : headers_) {
            outputBuffer->write_string(item.first);
            outputBuffer->write_string(": ");
            outputBuffer->write_string(item.second);
            outputBuffer->write_string("\r\n");
        }

        outputBuffer->write_string("\r\n");

        return true;
    }
}