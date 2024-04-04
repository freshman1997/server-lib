#include "net/base/connection/connection.h"

#include "net/http/context.h"
#include "net/http/request.h"
#include "net/http/request_parser.h"

namespace net::http 
{
    static const char* http_method_descs[9] = {
        "get",
        "post",
        "put",
        "delete",
        "option",
        "head",
        "comment",
        "trace",
        "patch",
    };

    HttpRequest::HttpRequest(HttpSessionContext *context) : HttpPacket(context)
    {
        parser_ = new HttpRequestParser(this);
        reset();
    }

    HttpRequest::~HttpRequest()
    {

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

    bool HttpRequest::pack_header()
    {
        auto outputBuffer = context_->get_connection()->get_output_buff();
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

        return true;
    }
}
