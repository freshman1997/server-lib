#include "response.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "context.h"
#include "header_key.h"
#include "net/connection/connection.h"
#include "response_code_desc.h"
#include "response_parser.h"

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
        const size_t len = strlen(data);
        if (len == 0) {
            return;
        }

        const auto buff = buffer::BufferedPool::get_instance()->allocate(len);
        buff->write_string(data);
        reader_.add_buffer(buff);
    }

    void HttpResponse::append_body(const std::string &data)
    {
        const auto buff = buffer::BufferedPool::get_instance()->allocate(data.size());
        buff->write_string(data);
        reader_.add_buffer(buff);
    }

    void HttpResponse::reset()
    {
        HttpPacket::reset();
        respCode_ = ResponseCode::bad_request;
    }

    bool HttpResponse::pack_header(Connection *conn)
    {
        if (!conn) {
            return false;
        }

        const auto descIt = responseCodeDescs.find(respCode_);
        if (descIt == responseCodeDescs.end() || respCode_ == ResponseCode::internal_server_error) {
            context_->process_error();
            return false;
        }

        const auto outputBuffer = conn ? conn->get_output_buff() : context_->get_connection()->get_output_buff();
        std::string header("HTTP/1.1");
        header.append(" ").append(descIt->second).append("\r\n");

        // 设置跨域头部
        add_header("Access-Control-Allow-Origin", "*");
        add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
        add_header("Access-Control-Max-Age", "86400"); // 24小时缓存

        if (!get_header(http_header_key::content_length)) {
            add_header("Content-Length", std::to_string(reader_.readable_bytes()));
        }
        
        for (const auto &[key, val] : headers_) {
            header.append(key).append(": ").append(val).append("\r\n");
        }

        header.append("\r\n");
        outputBuffer->write_string(header);

        return true;
    }

    void HttpResponse::process_error(ResponseCode errorCode) const
    {
        auto it = responseCodeDescs.find(errorCode);
        if (it == responseCodeDescs.end()) {
            errorCode = ResponseCode::internal_server_error;
            it = responseCodeDescs.find(errorCode);
        }

        const std::string msg = "<h1 style=\"margin:0 auto;display: flex;justify-content: center;\">"+ it->second +"</h1>";
        const std::string response = "HTTP/1.1 " + it->second + "\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;

        auto buff = context_->get_connection()->get_output_buff();
        buff->write_string(response);
        context_->get_connection()->flush();
        context_->get_connection()->close();
    }

    void HttpResponse::dispatch_task()
    {
        if (!task_) {
            return;
        }

        if (reader_.readable_bytes() > 0) {
            task_->on_data(reader_);
            reader_.init();
        }
        // TODO: dispatch task
    }
}