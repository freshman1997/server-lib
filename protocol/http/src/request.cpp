#include "base/time.h"
#include "net/connection/connection.h"
#include "context.h"
#include "request.h"
#include "request_parser.h"
#include "cookie.h"
#include "header_key.h"
#include "url.h"

#include <cctype>

namespace yuan::net::http 
{
    namespace
    {
        bool is_header_ows(char ch) noexcept
        {
            return ch == ' ' || ch == '\t';
        }

        bool token_equals_ci(std::string_view token, std::string_view expected)
        {
            if (token.size() != expected.size()) {
                return false;
            }
            for (std::size_t i = 0; i < token.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(token[i])) !=
                    std::tolower(static_cast<unsigned char>(expected[i]))) {
                    return false;
                }
            }
            return true;
        }
    }

    static const char* http_method_descs[] = {
        "GET",
        "POST",
        "PUT",
        "DELETE",
        "OPTIONS",
        "HEAD",
        "COMMENT",
        "TRACE",
        "PATCH",
        "PROPFIND",
        "PROPPATCH",
        "MKCOL",
        "COPY",
        "MOVE",
        "LOCK",
        "UNLOCK",
        "REPORT",
        "ACL",
        "SEARCH",
        nullptr  // sentinel
    };

    static constexpr std::string_view http_method_desc_views[] = {
        "GET",
        "POST", 
        "PUT",
        "DELETE",
        "OPTIONS",
        "HEAD",
        "COMMENT",
        "TRACE",
        "PATCH",
        "PROPFIND",
        "PROPPATCH",
        "MKCOL",
        "COPY",
        "MOVE",
        "LOCK",
        "UNLOCK",
        "REPORT",
        "ACL",
        "SEARCH",
        ""
    };

    HttpRequest::HttpRequest(HttpSessionContext *context) : HttpPacket(context)
    {
        parser_ = std::make_unique<HttpRequestParser>(this);
        reset();
    }

    HttpRequest::~HttpRequest()
    {
        if (task_) {
            task_->on_connection_close();
        }
    }

    std::string_view HttpRequest::get_method_string() const
    {
        if (method_ == HttpMethod::invalid_) return {};
        return http_method_desc_views[static_cast<int>(method_)];
    }

    std::string HttpRequest::get_raw_method() const
    {
        if (!is_ok()) return {};
        return std::string(get_method_string());
    }

    void HttpRequest::reset()
    {
        HttpPacket::reset();
        query_pos_ = std::string::npos;
        url_domain_.clear();
        url_domain_parsed_ = false;
        url_domain_valid_ = true;
        method_ = HttpMethod::invalid_;
        connection_close_requested_ = false;
        connection_keep_alive_requested_ = false;
        cookies_parsed_ = false;
        parsed_cookies_.clear();
    }

    const std::vector<std::string> &HttpRequest::get_url_domain() const
    {
        if (!url_domain_parsed_) {
            url_domain_.clear();
            url_domain_valid_ = url::decode_url_domain(url_, url_domain_);
            url_domain_parsed_ = true;
        }
        return url_domain_;
    }

    void HttpRequest::set_raw_url(std::string url)
    {
        url_ = std::move(url);
        query_pos_ = url_.find('?');
        url_domain_.clear();
        url_domain_parsed_ = false;
        url_domain_valid_ = true;
    }

    void HttpRequest::note_connection_header(std::string_view value)
    {
        if (value == "keep-alive") {
            connection_keep_alive_requested_ = true;
            return;
        }
        if (value == "close") {
            connection_close_requested_ = true;
            return;
        }

        std::size_t pos = 0;
        while (pos <= value.size()) {
            const auto comma = value.find(',', pos);
            auto end = comma == std::string_view::npos ? value.size() : comma;
            while (pos < end && is_header_ows(value[pos])) {
                ++pos;
            }
            while (end > pos && is_header_ows(value[end - 1])) {
                --end;
            }

            const auto token = value.substr(pos, end - pos);
            if (token_equals_ci(token, "close")) {
                connection_close_requested_ = true;
            } else if (token_equals_ci(token, "keep-alive")) {
                connection_keep_alive_requested_ = true;
            }

            if (comma == std::string_view::npos) {
                break;
            }
            pos = comma + 1;
        }
    }

    bool HttpRequest::pack_header(Connection *conn)
{
    auto *target = conn ? conn : context_->get_connection();
    if (!target) {
        return false;
    }

    auto msv = get_method_string();
    target->append_output(msv);
    target->append_output(" ");
    target->append_output(url_.empty() ? std::string_view("/") : std::string_view(url_));
    target->append_output(" HTTP/1.1\r\n");

    for (const auto &item : headers_) {
        target->append_output(item.first);
        target->append_output(": ");
        target->append_output(item.second);
        target->append_output("\r\n");
    }

    target->append_output("\r\n");
    target->flush();

    return true;
}

    std::string HttpRequest::get_last_uri()
    {
        if (url_.empty()) {
            return "tmp." + std::to_string(yuan::base::time::now());
        }
        auto pos = url_.find_last_of('/');
        return (pos != std::string::npos) ? url_.substr(pos + 1) : url_;
    }

    std::string_view HttpRequest::get_path() const
    {
        if (url_.empty()) return {};
        return std::string_view(url_.data(), query_pos_ == std::string::npos ? url_.size() : query_pos_);
    }

    std::string_view HttpRequest::get_query_string() const
    {
        if (url_.empty()) return {};
        if (query_pos_ == std::string::npos || query_pos_ + 1 >= url_.size()) return {};
        return std::string_view(url_.data() + query_pos_ + 1, url_.size() - query_pos_ - 1);
    }

    std::string HttpRequest::get_param(const std::string &key, const std::string &default_val) const
    {
        auto it = params_.find(key);
        if (it != params_.end() && !it->second.empty()) {
            return it->second.front();
        }
        return default_val;
    }

    int HttpRequest::get_param_int(const std::string &key, int default_val) const
    {
        auto str = get_param(key, "");
        if (str.empty()) return default_val;
        try { return std::stoi(str); } catch (...) { return default_val; }
    }

    const std::unordered_map<std::string, std::string> & HttpRequest::cookies() const
    {
        if (!cookies_parsed_) {
            const std::string *cookie_header = get_header(http_header_key::cookie);
            parsed_cookies_ = CookieParser::parse(cookie_header ? *cookie_header : "");
            cookies_parsed_ = true;
        }
        return parsed_cookies_;
    }

    std::string HttpRequest::get_cookie(const std::string &name) const
    {
        const auto &cks = cookies();
        auto it = cks.find(name);
        return (it != cks.end()) ? it->second : "";
    }
}
