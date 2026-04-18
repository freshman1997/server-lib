#include "base/time.h"
#include "net/connection/connection.h"
#include "context.h"
#include "request.h"
#include "request_parser.h"
#include "cookie.h"
#include "header_key.h"

namespace yuan::net::http 
{
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
        url_domain_.clear();
        method_ = HttpMethod::invalid_;
        cookies_parsed_ = false;
        parsed_cookies_.clear();
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
    context_->get_connection()->flush();

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
        auto pos = url_.find_first_of('?');
        return std::string_view(url_.data(), pos == std::string::npos ? url_.size() : pos);
    }

    std::string_view HttpRequest::get_query_string() const
    {
        if (url_.empty()) return {};
        auto pos = url_.find_first_of('?');
        if (pos == std::string::npos) return {};
        return std::string_view(url_.data() + pos + 1, url_.size() - pos - 1);
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

