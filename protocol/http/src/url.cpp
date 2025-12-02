#include <iomanip>
#include <sstream>

#include "url.h"

namespace yuan::url 
{
    std::string url_encode(const std::string &str)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (const auto c : str) {
            // 保留字符不编码
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            }
            // 其他字符编码为 %xx 形式
            else {
                escaped << std::uppercase;
                escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
                escaped << std::nouppercase;
            }
        }

        return escaped.str();
    }

    std::string url_decode(const std::string &str, const bool is_query_string)
    {
        return url_decode(str.c_str(), str.c_str() + str.size(), is_query_string);
    }

    std::string url_decode(const char *begin, const char *end, const bool is_query_string)
    {
        std::string result;
        result.reserve(end - begin);

        for (const char *str = begin; str < end; ++str) {
            if (*str == '%' && str + 2 < end) {
                // 处理百分号编码
                std::istringstream hex_stream(std::string(str + 1, str + 3));
                if (int hex_value; hex_stream >> std::hex >> hex_value && hex_value >= 0 && hex_value <= 255) {
                    // 将解码后的字符加入结果
                    result += static_cast<char>(hex_value);
                    str += 2;
                } else {
                    // 如果解析失败，保持原样
                    result += *str;
                }
            } else if (is_query_string && *str == '+') {
                // 只有在查询字符串中才将 + 替换为空格
                result += ' ';
            } else {
                result += *str;
            }
        }
        return result;
    }

    bool decode_url_domain(const std::string &url, std::vector<std::string> &urlDomain)
    {
        size_t pos = url.find_first_of("/");
        if (pos == std::string::npos) {
            return false;
        }

        if (url.size() == 1) {
            urlDomain.push_back("/");
        }

        size_t i = pos + 1;
        size_t sz = url.size();
        for (; i < sz; ++i) {
            size_t j = i;
            std::string domain;
            for (; j < sz && url[j] != '/' && url[j] != '?'; ++j) {
                domain.push_back(url[j]);
            }

            urlDomain.push_back(url_decode(domain.c_str(), domain.c_str() + domain.size()));

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

    bool decode_parameters(const std::string &url, std::unordered_map<std::string, std::vector<std::string>> &params, bool fromBody)
    {
        size_t pos = url.find_first_of("?");
        if (pos == std::string::npos) {
            if (!fromBody) {
                return true;
            }
            
            pos = 0;
        } else {
            pos += 1;
        }

        size_t sz = url.size();
        size_t i = pos;
        for (; i < sz; ++i) {
            std::string key;
            size_t j = i;
            for (; j < sz && url[j] != '='; ++j) {
                key.push_back(url[j]);
            }

            if (j >= sz || url[j] != '=' || key.empty()) {
                return false;
            }

            key = url_decode(key.c_str(), key.c_str() + key.size());
            
            while (j < sz) {
                std::string val;
                for (++j; j < sz && url[j] != '#' && url[j] != '&'; ++j) {
                    val.push_back(url[j]);
                }

                if (!val.empty()) {
                    val = url_decode(val.c_str(), val.c_str() + val.size());
                    params[key].push_back(val);
                }

                if (url[j] == '&') {
                    break;
                }

                if (j < sz && url[j] != '#') {
                    return false;
                }
            }

            i = j;
        }

        return i >= sz;
    }

    bool decode_url(const std::string &rawUrl, UrlDetail &url)
    {
        auto idx = rawUrl.find(":");
        if (idx == std::string::npos) {
            return false;
        }

        url.protocol_ = rawUrl.substr(0, idx);

        // aa
        idx = rawUrl.find("//", idx);
        if (idx == std::string::npos) {
            return false;
        }

        int from = idx + 2;
        idx = rawUrl.find("/", from);
        if (idx == std::string::npos) {
            return false;
        }

        const std::string &tmpDomain = rawUrl.substr(from, idx - from);
        url.domain_ = url_decode(tmpDomain);

        std::string tmp = rawUrl.substr(idx);
        if (!decode_url_domain(tmp, url.uri_)) {
            return false;
        }

        if (!decode_parameters(tmp, url.parameters_)) {
            return false;
        }

        return true;
    }

    bool encode_url(const UrlDetail &url, std::string &result)
    {
        result += url.protocol_ + "://" + url_encode(url.domain_) + "/";
        for (int i = 0; i < url.uri_.size(); ++i) {
            result += url.uri_[i];
            if (i < url.uri_.size() - 1) {
                result += "/";
            }
        }

        if (!url.parameters_.empty()) {
            result += "?";
            int c = 0;
            for (const auto &item : url.parameters_) {
                result += url_encode(item.first) + "=";
                for (int i = 0; i < item.second.size(); ++i) {
                    result += url_encode(item.second[i]);
                    if (i < item.second.size() - 1) {
                        result += '#';
                    }
                }

                if (c < url.parameters_.size() - 1) {
                    result += '&';
                }

                ++c;
            }
        }

        return true;
    }
}