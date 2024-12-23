#include "net/connection/connection.h"
#include "response_parser.h"
#include "response.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "context.h"
#include "response_code_desc.h"

namespace yuan::net::http
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

    bool HttpResponse::pack_header(Connection *conn)
    {
        auto descIt = responseCodeDescs.find(respCode_);
        if (descIt == responseCodeDescs.end() || respCode_ == ResponseCode::internal_server_error) {
            context_->process_error();
            return false;
        }

        auto outputBuffer = conn ? conn->get_output_buff() : context_->get_connection()->get_output_buff();
        std::string header("HTTP/1.1");
        header.append(" ").append(descIt->second).append("\r\n");

        for (const auto &item : headers_) {
            header.append(item.first).append(": ").append(item.second).append("\r\n");
        }

        header.append("\r\n");
        outputBuffer->write_string(header);

        return true;
    }

    void HttpResponse::process_error(ResponseCode errorCode)
    {
        auto it = responseCodeDescs.find(errorCode);
        if (it == responseCodeDescs.end()) {
            errorCode = ResponseCode::internal_server_error;
            it = responseCodeDescs.find(errorCode);
        }

        std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">"+ it->second +"</h1>";;
        std::string response = "HTTP/1.1 " + it->second + "\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;

        auto buff = context_->get_connection()->get_output_buff();
        buff->write_string(response);
        context_->get_connection()->flush();
        context_->get_connection()->close();
    }
}