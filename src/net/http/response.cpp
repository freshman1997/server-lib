#include "net/base/connection/connection.h"
#include "net/http/response_parser.h"
#include "net/http/response.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "net/http/context.h"
#include "net/http/response_code_desc.h"
#include "singleton/singleton.h"

namespace net::http
{
    HttpResponse::HttpResponse(HttpSessionContext *context) : HttpPacket(context)
    {
        parser_ = new HttpResponseParser(this);
        reset();
    }

    HttpResponse::~HttpResponse()
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
        HttpPacket::reset();
        respCode_ = ResponseCode::bad_request;
        buffer_->reset();
    }

    bool HttpResponse::pack_header()
    {
        auto descIt = responseCodeDescs.find(respCode_);
        if (descIt == responseCodeDescs.end() || respCode_ == ResponseCode::internal_server_error) {
            context_->process_error();
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