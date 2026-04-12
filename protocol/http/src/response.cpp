#include "net/connection/connection.h"
#include "response_parser.h"
#include "response.h"
#include "context.h"
#include "response_code_desc.h"
#include "cookie.h"
#include "sse.h"

namespace yuan::net::http
{
    HttpResponse::HttpResponse(HttpSessionContext *context) : HttpPacket(context)
    {
        parser_ = new HttpResponseParser(this);
        reset();
    }

    HttpResponse::~HttpResponse() {}

    void HttpResponse::append_body(std::string_view data)
    {
        HttpPacket::append_body(data);
    }

    void HttpResponse::append_body(const char *data)
    {
        if (data) {
            HttpPacket::append_body(std::string_view(data, std::strlen(data)));
        }
    }

    void HttpResponse::append_body(const std::string &data)
    {
        HttpPacket::append_body(data);
    }

    void HttpResponse::append_body(std::string &&data)
    {
        HttpPacket::append_body(data);
    }

    void HttpResponse::reset()
    {
        HttpPacket::reset();
        respCode_ = ResponseCode::bad_request;
        buffer_.clear();
        is_sse_ = false;
        sse_event_count_ = 0;
    }

    bool HttpResponse::pack_header(Connection *conn)
{
    auto descIt = responseCodeDescs.find(respCode_);
    if (descIt == responseCodeDescs.end() || respCode_ == ResponseCode::internal_server_error) {
        context_->process_error();
        return false;
    }

    auto *target = conn ? conn : context_->get_connection();
    if (!target) {
        return false;
    }

    target->append_output("HTTP/1.1 ");
    target->append_output(descIt->second);
    target->append_output("\r\n");

    for (const auto &item : headers_) {
        target->append_output(item.first);
        target->append_output(": ");
        target->append_output(item.second);
        target->append_output("\r\n");
    }

    target->append_output("\r\n");
    return true;
}

    void HttpResponse::process_error(ResponseCode errorCode)
{
    auto it = responseCodeDescs.find(errorCode);
    if (it == responseCodeDescs.end()) {
        errorCode = ResponseCode::internal_server_error;
        it = responseCodeDescs.find(errorCode);
    }

    const std::string msg = "<h1 style=\"margin:0 auto;display:flex;justify-content:center;\">" + it->second + "</h1>";

    auto *conn = context_->get_connection();
    if (!conn) {
        return;
    }

    conn->append_output("HTTP/1.1 ");
    conn->append_output(it->second);
    conn->append_output("\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: ");
    conn->append_output(std::to_string(msg.size()));
    conn->append_output("\r\n\r\n");
    conn->append_output(msg);

    conn->flush();
    conn->close();
}

    void HttpResponse::dispatch_task()
    {
        if (!task_) return;

        if (buffer_.readable_bytes() > 0) {
            auto body = buffer_.copy_readable();
            task_->on_data(body);
            buffer_.clear();
        }
    }

    void HttpResponse::json(const std::string &json_str, ResponseCode code)
    {
        set_response_code(code);
        add_header("Content-Type", "application/json; charset=utf-8");
        append_body(json_str);
        add_header("Content-Length", std::to_string(body_buffer_size()));
    }

    void HttpResponse::redirect(const std::string &url, ResponseCode code)
    {
        set_response_code(code);
        add_header("Location", url);
        // 常见重定向状态码: 301, 302, 303, 307, 308
        switch (code) {
            case ResponseCode::moved_permanently:   // 301
            case ResponseCode::found:               // 302  
            case ResponseCode::see_other:           // 303
                break;
            default:
                set_response_code(ResponseCode::found);  // 默认302
                break;
        }
    }

    void HttpResponse::set_cookie(const std::string &name, const std::string &value,
                                   int64_t max_age, const std::string &path,
                                   const std::string &domain, bool http_only,
                                   bool secure, const std::string &same_site)
    {
        SetCookieBuilder builder(name, value);
        if (!path.empty()) builder.set_path(path);
        if (!domain.empty()) builder.set_domain(domain);
        if (max_age >= 0) builder.set_max_age(max_age);
        if (http_only) builder.set_http_only(true);
        if (secure) builder.set_secure(true);
        if (!same_site.empty()) builder.set_same_site(same_site);
        
        add_header("Set-Cookie", builder.build());
    }

    void HttpResponse::set_sse()
    {
        is_sse_ = true;
        set_response_code(ResponseCode::ok_);
        add_header("Content-Type", "text/event-stream");
        add_header("Cache-Control", "no-cache, no-transform");
        add_header("Connection", "keep-alive");
        add_header("X-Accel-Buffering", "no");
    }

    void HttpResponse::send_sse_event(const std::string &event, const std::string &data, const std::string &id)
    {
        SseEvent e;
        e.event = event;
        e.data = data;
        e.id = id;
        ++sse_event_count_;
        append_body(e.serialize());

        // SSE事件需要立即发�?        pack_and_send(context_->get_connection());
    }
}






