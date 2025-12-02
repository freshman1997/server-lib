#include "base/time.h"
#include "net/connection/connection.h"
#include "context.h"
#include "request.h"
#include "request_parser.h"

namespace yuan::net::http 
{
    static const char* http_method_descs[9] = {
        "GET",
        "POST",
        "PUT",
        "DELETE",
        "OPTION",
        "HEAD",
        "COMMENT",
        "TRACE",
        "PATCH",
    };

    HttpRequest::HttpRequest(HttpSessionContext *context) : HttpPacket(context)
    {
        parser_ = new HttpRequestParser(this);
        reset();
    }

    HttpRequest::~HttpRequest()
    {
        if (task_) {
            task_->on_connection_close();
        }
    }

    HttpMethod HttpRequest::get_method() const
    {
        return this->method_;
    }

    std::string HttpRequest::get_raw_method() const
    {
        if (!is_ok()) {
            return {};
        }

        return http_method_descs[(uint32_t)method_];
    }

    void HttpRequest::reset()
    {
        HttpPacket::reset();
        url_domain_.clear();
        method_ = HttpMethod::invalid_;
    }

    bool HttpRequest::pack_header(Connection *conn)
    {
        auto outputBuffer = conn ? conn->get_output_linked_buffer()->get_current_buffer() : context_->get_connection()->get_output_linked_buffer()->get_current_buffer();
        const std::string &method = get_raw_method();
        std::string header = method.empty() ? "GET" : method;
        header.append(" ");
        header.append(url_.empty() ? "/" : url_);
        header.append(" ");
        header.append("HTTP/1.1\r\n");

        for (const auto &item : headers_) {
            header.append(item.first).append(": ").append(item.second).append("\r\n");
        }

        header.append("\r\n");
        outputBuffer->write_string(header);
        
        context_->get_connection()->flush();

        return true;
    }

    std::string HttpRequest::get_last_uri()
    {
        if (url_.empty()) {
            return "tmp." + std::to_string(yuan::base::time::now());
        }
        return url_.substr(url_.find_last_of("/") + 1);
    }
}
