#ifndef __HTTP_COOKIE_H__
#define __HTTP_COOKIE_H__

#include <string>
#include <unordered_map>
#include <string_view>
#include <cstdint>

namespace yuan::net::http
{
    // Cookie 解析器
    class CookieParser
    {
    public:
        // 从Cookie头部值解析出所有 key=value 对
        static std::unordered_map<std::string, std::string> parse(std::string_view cookie_header);
        
        // 获取单个cookie值
        static std::string get(std::string_view cookie_header, const std::string &name);

    private:
        static std::string_view trim(std::string_view sv);
    };

    // Set-Cookie 构建器
    class SetCookieBuilder
    {
    public:
        SetCookieBuilder(const std::string &name, const std::string &value);

        SetCookieBuilder& set_path(const std::string &path);
        SetCookieBuilder& set_domain(const std::string &domain);
        SetCookieBuilder& set_max_age(int64_t seconds);
        SetCookieBuilder& set_expires(const std::string &expires);  // GMT格式
        SetCookieBuilder& set_http_only(bool flag = true);
        SetCookieBuilder& set_secure(bool flag = true);
        SetCookieBuilder& set_same_site(const std::string &mode);  // Strict/Lax/None

        // 构造完整的Set-Cookie头值
        std::string build() const;

    private:
        std::string name_;
        std::string value_;
        std::string path_ = "/";
        std::string domain_;
        int64_t max_age_ = -1;
        std::string expires_;
        bool http_only_ = true;
        bool secure_ = false;
        std::string same_site_ = "Lax";
    };
}

#endif
