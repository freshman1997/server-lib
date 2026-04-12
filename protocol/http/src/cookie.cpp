#include "cookie.h"
#include <sstream>

namespace yuan::net::http
{
    // ==================== CookieParser ====================

    std::string_view CookieParser::trim(std::string_view sv)
    {
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
            sv.remove_prefix(1);
        }
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) {
            sv.remove_suffix(1);
        }
        return sv;
    }

    std::unordered_map<std::string, std::string> CookieParser::parse(std::string_view cookie_header)
    {
        std::unordered_map<std::string, std::string> cookies;
        
        if (cookie_header.empty()) return cookies;

        size_t pos = 0;
        const size_t len = cookie_header.size();

        while (pos < len) {
            // 跳过空格和分号
            while (pos < len && (cookie_header[pos] == ' ' || cookie_header[pos] == ';')) ++pos;
            
            // 找到key
            size_t key_start = pos;
            while (pos < len && cookie_header[pos] != '=') {
                if (cookie_header[pos] == ';') {  // 没有value的cookie
                    auto key = trim(std::string_view(cookie_header.data() + key_start, pos - key_start));
                    if (!key.empty()) cookies[std::string(key)] = "";
                    ++pos;  // 跳过分号
                    break;
                }
                ++pos;
            }

            if (pos >= len) break;

            std::string key(trim(std::string_view(cookie_header.data() + key_start, pos - key_start)));
            if (cookie_header[pos] != ';') {
                ++pos;  // 跳过'='
                
                // 找到value（以分号或结尾结束）
                size_t val_start = pos;
                bool quoted = (pos < len && cookie_header[pos] == '"');
                if (quoted) {
                    ++val_start;  // 跳过开引号
                    ++pos;
                    
                    // 找到闭合引号
                    while (pos < len && cookie_header[pos] != '"') ++pos;
                    auto val = std::string_view(cookie_header.data() + val_start, pos - val_start);
                    if (!key.empty()) cookies[std::move(key)] = std::string(val);
                    
                    if (pos < len) ++pos;  // 跳过闭引号
                    continue;
                }

                while (pos < len && cookie_header[pos] != ';') ++pos;
                auto val = trim(std::string_view(cookie_header.data() + val_start, pos - val_start));
                if (!key.empty()) cookies[std::move(key)] = std::string(val);
            } else if (!key.empty()) {
                cookies[std::move(key)] = "";
            }
        }

        return cookies;
    }

    std::string CookieParser::get(std::string_view cookie_header, const std::string &name)
    {
        auto cookies = parse(cookie_header);
        auto it = cookies.find(name);
        return (it != cookies.end()) ? it->second : "";
    }

    // ==================== SetCookieBuilder ====================

    SetCookieBuilder::SetCookieBuilder(const std::string &name, const std::string &value)
        : name_(name), value_(value) {}

    SetCookieBuilder& SetCookieBuilder::set_path(const std::string &path) { path_ = path; return *this; }
    SetCookieBuilder& SetCookieBuilder::set_domain(const std::string &domain) { domain_ = domain; return *this; }
    SetCookieBuilder& SetCookieBuilder::set_max_age(int64_t seconds) { max_age_ = seconds; return *this; }
    SetCookieBuilder& SetCookieBuilder::set_expires(const std::string &expires) { expires_ = expires; return *this; }
    SetCookieBuilder& SetCookieBuilder::set_http_only(bool flag) { http_only_ = flag; return *this; }
    SetCookieBuilder& SetCookieBuilder::set_secure(bool flag) { secure_ = flag; return *this; }
    SetCookieBuilder& SetCookieBuilder::set_same_site(const std::string &mode) { same_site_ = mode; return *this; }

    std::string SetCookieBuilder::build() const
    {
        std::ostringstream oss;
        oss << name_ << "=" << value_;
        
        if (!path_.empty()) oss << "; Path=" << path_;
        if (!domain_.empty()) oss << "; Domain=" << domain_;
        if (max_age_ >= 0) oss << "; Max-Age=" << max_age_;
        if (!expires_.empty()) oss << "; Expires=" << expires_;
        if (http_only_) oss << "; HttpOnly";
        if (secure_) oss << "; Secure";
        if (!same_site_.empty()) oss << "; SameSite=" << same_site_;

        return oss.str();
    }
}
