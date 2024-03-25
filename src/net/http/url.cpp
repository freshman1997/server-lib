#include <iomanip>
#include <sstream>

#include "net/http/url.h"

namespace url 
{
    std::string url_encode(const std::string &str)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (auto c : str) {
            // 保留字符不编码
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            }
            // 其他字符编码为 %xx 形式
            else {
                escaped << std::uppercase;
                escaped << '%' << std::setw(2) << int((unsigned char) c);
                escaped << std::nouppercase;
            }
        }

        return escaped.str();
    }

    std::string url_decode(const std::string &str)
    {
        std::ostringstream unescaped;

        for (std::string::size_type i = 0; i < str.length(); ++i) {
            if (str[i] == '%') {
                // 解码 %xx 形式的字符
                std::string hex = str.substr(i + 1, 2);
                char decoded = std::stoi(hex, nullptr, 16);
                unescaped << decoded;
                i += 2;
            }
            else if (str[i] == '+') {
                // 解码空格
                unescaped << ' ';
            }
            else {
                // 保留字符不解码
                unescaped << str[i];
            }
        }

        return unescaped.str();
    }

    bool decode_url_domain(const std::string &url, std::vector<std::string> &urlDomain)
    {
        size_t pos = url.find_first_of("/");
        if (pos == std::string::npos) {
            return false;
        }

        urlDomain.push_back("/");
        size_t i = pos + 1;
        size_t sz = url.size();
        for (; i < sz; ++i) {
            size_t j = i;
            std::string domain;
            for (; j < sz && url[j] != '/' && url[j] != '?'; ++j) {
                domain.push_back(url[j]);
            }

            urlDomain.push_back(domain);
            i = j;
            if (url[j] == '?') {
                break;
            }
        }

        if (i < sz && url[i] != '?') {
            return false;
        }

        return true;
    }

    bool decode_parameters(const std::string &url, std::unordered_map<std::string, std::string> &headers)
    {
        size_t pos = url.find_first_of("?");
        if (pos == std::string::npos) {
            return true;
        }

        size_t sz = url.size();
        size_t i = pos + 1;
        for (; i < sz; ++i) {
            std::string key;
            size_t j = i;
            for (; j < sz && url[j] != '='; ++j) {
                key.push_back(url[j]);
            }

            if (j >= sz || url[j] != '=' || key.empty()) {
                return false;
            }

            std::string val;
            for (++j; j < sz && url[j] != '&'; ++j) {
                val.push_back(url[j]);
            }

            headers[key] = val;
            i = j;
        }

        return i >= sz;
    }
}