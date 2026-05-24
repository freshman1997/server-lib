#include "response.h"

#include "context.h"
#include "cookie.h"
#include "net/connection/connection.h"
#include "header_key.h"
#include "http_headers.h"
#include "response_code_desc.h"
#include "response_parser.h"
#include "sse.h"

#include <charconv>
#include <cstring>
#include <string_view>

namespace yuan::net::http
{
    namespace
    {
        std::string_view fast_status_line(ResponseCode code) noexcept
        {
            switch (code) {
            case ResponseCode::ok_:
                return "HTTP/1.1 200 OK\r\n";
            case ResponseCode::not_found:
                return "HTTP/1.1 404 Not Found\r\n";
            case ResponseCode::bad_request:
                return "HTTP/1.1 400 Bad Request\r\n";
            case ResponseCode::no_content:
                return "HTTP/1.1 204 No Content\r\n";
            case ResponseCode::found:
                return "HTTP/1.1 302 Found\r\n";
            default:
                return {};
            }
        }

        std::string_view cached_simple_response_header(ResponseCode code,
                                                       std::string_view content_type,
                                                       std::size_t body_size)
        {
            struct Cache
            {
                ResponseCode code = ResponseCode::invalid;
                std::string content_type;
                std::size_t body_size = static_cast<std::size_t>(-1);
                std::string header;
            };

            const auto fast_line = fast_status_line(code);
            if (fast_line.empty()) {
                return {};
            }

            thread_local Cache cache;
            if (cache.code == code &&
                cache.body_size == body_size &&
                cache.content_type.size() == content_type.size() &&
                std::string_view(cache.content_type) == content_type) {
                return cache.header;
            }

            char length_buf[32];
            auto [length_end, ec] = std::to_chars(length_buf, length_buf + sizeof(length_buf), body_size);
            if (ec != std::errc{}) {
                return {};
            }
            const std::string_view length_text(length_buf, static_cast<std::size_t>(length_end - length_buf));

            cache.code = code;
            cache.content_type.assign(content_type);
            cache.body_size = body_size;
            cache.header.clear();
            cache.header.reserve(fast_line.size() +
                                 std::string_view("Content-Type: ").size() + content_type.size() + 2 +
                                 std::string_view("Content-Length: ").size() + length_text.size() + 4);
            cache.header.append(fast_line);
            if (!content_type.empty()) {
                cache.header.append("Content-Type: ");
                cache.header.append(content_type);
                cache.header.append("\r\n");
            }
            cache.header.append("Content-Length: ");
            cache.header.append(length_text);
            cache.header.append("\r\n\r\n");
            return cache.header;
        }
    }

    HttpResponse::HttpResponse(HttpSessionContext *context) : HttpPacket(context)
    {
        parser_ = std::make_unique<HttpResponseParser>(this);
        reset();
    }

    HttpResponse::~HttpResponse() = default;

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

    void HttpResponse::send_body(std::string_view body, std::string_view content_type, ResponseCode code)
    {
        set_response_code(code);
        auto *conn = context_ ? context_->get_connection() : nullptr;
        if (!conn) {
            return;
        }

        if (headers_.empty()) {
            if (const auto cached_header = cached_simple_response_header(respCode_, content_type, body.size());
                !cached_header.empty()) {
                thread_local std::string payload;
                const std::size_t payload_size = cached_header.size() + body.size();
                payload.clear();
                if (payload.capacity() < payload_size) {
                    payload.reserve(payload_size);
                }
                payload.append(cached_header);
                payload.append(body);
                conn->write_raw_and_flush(payload);
                headers_sent_ = true;
                return;
            }
        }

        const auto fast_line = fast_status_line(respCode_);
        decltype(responseCodeDescs)::const_iterator descIt;
        if (fast_line.empty()) {
            descIt = responseCodeDescs.find(respCode_);
            if (descIt == responseCodeDescs.end() || respCode_ == ResponseCode::internal_server_error) {
                context_->process_error();
                return;
            }
        }

        char length_buf[32];
        auto [length_end, ec] = std::to_chars(length_buf, length_buf + sizeof(length_buf), body.size());
        if (ec != std::errc{}) {
            context_->process_error();
            return;
        }
        const std::string_view length_text(length_buf, static_cast<std::size_t>(length_end - length_buf));

        std::size_t payload_size = 2 + body.size();
        payload_size += !fast_line.empty()
            ? fast_line.size()
            : std::string_view("HTTP/1.1 ").size() + descIt->second.size() + 2;
        if (!content_type.empty()) {
            payload_size += std::string_view("Content-Type: ").size() + content_type.size() + 2;
        }
        payload_size += std::string_view("Content-Length: ").size() + length_text.size() + 2;
        for (const auto &item : headers_) {
            if (header_key_equals_ci(item.first, http_header_key::content_type) ||
                header_key_equals_ci(item.first, http_header_key::content_length)) {
                continue;
            }
            payload_size += item.first.size() + 2 + item.second.size() + 2;
        }

        thread_local std::string payload;
        payload.clear();
        if (payload.capacity() < payload_size) {
            payload.reserve(payload_size);
        }
        if (!fast_line.empty()) {
            payload.append(fast_line);
        } else {
            payload.append("HTTP/1.1 ");
            payload.append(descIt->second);
            payload.append("\r\n");
        }
        if (!content_type.empty()) {
            payload.append("Content-Type: ");
            payload.append(content_type);
            payload.append("\r\n");
        }
        payload.append("Content-Length: ");
        payload.append(length_text);
        payload.append("\r\n");
        for (const auto &item : headers_) {
            if (header_key_equals_ci(item.first, http_header_key::content_type) ||
                header_key_equals_ci(item.first, http_header_key::content_length)) {
                continue;
            }
            payload.append(item.first);
            payload.append(": ");
            payload.append(item.second);
            payload.append("\r\n");
        }
        payload.append("\r\n");
        payload.append(body);

        conn->write_raw_and_flush(payload);
        headers_sent_ = true;
    }

    void HttpResponse::reset()
    {
        HttpPacket::reset();
        respCode_ = ResponseCode::bad_request;
        buffer_.clear();
        is_sse_ = false;
        headers_sent_ = false;
        sse_event_count_ = 0;
    }

    bool HttpResponse::pack_header(Connection *conn)
    {
        const auto fast_line = fast_status_line(respCode_);
        decltype(responseCodeDescs)::const_iterator descIt;
        if (fast_line.empty()) {
            descIt = responseCodeDescs.find(respCode_);
            if (descIt == responseCodeDescs.end() || respCode_ == ResponseCode::internal_server_error) {
                context_->process_error();
                return false;
            }
        }

        auto *target = conn ? conn : context_->get_connection();
        if (!target) {
            return false;
        }

        std::size_t header_size = 2;
        if (!fast_line.empty()) {
            header_size += fast_line.size();
        } else {
            header_size += std::string_view("HTTP/1.1 ").size() + descIt->second.size() + 2;
        }
        for (const auto &item : headers_) {
            header_size += item.first.size() + 2 + item.second.size() + 2;
        }

        thread_local std::string header;
        header.clear();
        if (header.capacity() < header_size) {
            header.reserve(header_size);
        }
        if (!fast_line.empty()) {
            header.append(fast_line);
        } else {
            header.append("HTTP/1.1 ");
            header.append(descIt->second);
            header.append("\r\n");
        }

        for (const auto &item : headers_) {
            header.append(item.first);
            header.append(": ");
            header.append(item.second);
            header.append("\r\n");
        }
        header.append("\r\n");

        target->append_output(header);
        headers_sent_ = true;
        return true;
    }

    void HttpResponse::process_error(ResponseCode errorCode)
    {
        set_response_code(errorCode);

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
        if (!task_) {
            return;
        }

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
        switch (code) {
        case ResponseCode::moved_permanently:
        case ResponseCode::found:
        case ResponseCode::see_other:
            break;
        default:
            set_response_code(ResponseCode::found);
            break;
        }
    }

    void HttpResponse::set_cookie(const std::string &name,
                                  const std::string &value,
                                  int64_t max_age,
                                  const std::string &path,
                                  const std::string &domain,
                                  bool http_only,
                                  bool secure,
                                  const std::string &same_site)
    {
        SetCookieBuilder builder(name, value);
        if (!path.empty()) {
            builder.set_path(path);
        }
        if (!domain.empty()) {
            builder.set_domain(domain);
        }
        if (max_age >= 0) {
            builder.set_max_age(max_age);
        }
        if (http_only) {
            builder.set_http_only(true);
        }
        if (secure) {
            builder.set_secure(true);
        }
        if (!same_site.empty()) {
            builder.set_same_site(same_site);
        }
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
        auto *conn = context_ ? context_->get_connection() : nullptr;
        if (!conn) {
            return;
        }

        if (!is_sse_) {
            set_sse();
        }

        SseEvent e;
        e.event = event;
        e.data = data;
        e.id = id;
        const std::string payload = e.serialize();

        if (!headers_sent_) {
            append_body(payload);
            pack_and_send(conn);
        } else {
            conn->append_output(payload);
            conn->flush();
        }
        ++sse_event_count_;
    }
}
