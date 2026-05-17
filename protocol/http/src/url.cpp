#include <cctype>
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
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << std::uppercase;
                escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
                escaped << std::nouppercase;
            }
        }
        return escaped.str();
    }

    // ============================================================
    // 快速 hex 解析（替代 istringstream，避免每次构造流）
    // ============================================================
    static inline int hex_val(char ch) noexcept
    {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    std::string url_decode(const std::string &str, const bool is_query_string)
    {
        return url_decode(str.c_str(), str.c_str() + str.size(), is_query_string);
    }

    std::string url_decode(const char *begin, const char *end, const bool is_query_string)
    {
        std::string result;
        result.reserve(end - begin);

        const char *p = begin;
        while (p < end) {
            if (*p == '%' && p + 2 < end) {
                int hi = hex_val(p[1]);
                int lo = hex_val(p[2]);
                if (hi >= 0 && lo >= 0) {
                    result.push_back(static_cast<char>((hi << 4) | lo));
                    p += 3;
                    continue;
                }
                // 非法 %xx，原样输出 %
            }

            if (is_query_string && *p == '+') {
                result.push_back(' ');
            } else {
                result.push_back(*p);
            }
            ++p;
        }
        return result;
    }

    bool decode_url_domain(const std::string &url, std::vector<std::string> &urlDomain)
    {
        size_t pos = url.find_first_of('/');
        if (pos == std::string::npos) {
            return false;
        }

        if (url.size() == pos + 1) {
            urlDomain.emplace_back("/");
            return true;
        }

        const bool needs_decode = url.find('%', pos + 1) != std::string::npos;
        size_t i = pos + 1;
        size_t sz = url.size();
        for (; i < sz; ++i) {
            size_t j = i;
            while (j < sz && url[j] != '/' && url[j] != '?') ++j;

            if (j > i) {
                if (needs_decode) {
                    urlDomain.push_back(url_decode(url.c_str() + i, url.c_str() + j));
                } else {
                    urlDomain.emplace_back(url.c_str() + i, j - i);
                }
            }

            i = j;
            if (j < sz && url[j] == '?') {
                break;
            }
        }

        if (i < sz && url[i] != '?' && url[i] != '/') {
            return false;
        }
        return true;
    }

    bool decode_parameters(const std::string &url,
                           std::unordered_map<std::string, std::vector<std::string>> &params,
                           bool fromBody)
    {
        size_t pos = url.find_first_of('?');
        if (pos == std::string::npos) {
            if (!fromBody) return true;
            pos = 0;
        } else {
            ++pos; // 跳过 '?'
        }

        size_t sz = url.size();
        size_t i = pos;

        while (i < sz) {
            // 解析 key
            size_t eq_pos = url.find_first_of('=', i);
            if (eq_pos == std::string::npos || eq_pos == i) {
                // 没有 '=' 或 key 为空
                if (!fromBody) break;
                return i >= sz;  // body 模式下要求严格格式
            }

            std::string key = url_decode(url.c_str() + i, url.c_str() + eq_pos);

            // 解析 value(s) —— 支持标准 '&' 分隔的多值
            i = eq_pos + 1;
            while (i < sz) {
                size_t next = url.find_first_of('&', i);
                if (next == std::string::npos) next = sz;

                std::string val = url_decode(url.c_str() + i, url.c_str() + next);
                params[key].push_back(std::move(val));

                if (next < sz) {
                    i = next + 1; // 跳过 '&'
                } else {
                    i = sz;
                    break;
                }
            }
        }
        return true;
    }

    bool decode_url(const std::string &rawUrl, UrlDetail &url)
    {
        auto idx = rawUrl.find(':');
        if (idx == std::string::npos || idx == 0) {
            return false;
        }

        url.protocol_ = rawUrl.substr(0, idx);

        idx = rawUrl.find("//", idx);
        if (idx == std::string::npos) {
            return false;
        }

        size_t from = idx + 2;
        idx = rawUrl.find('/', from);
        if (idx == std::string::npos) {
            // 无路径的情况: http://example.com
            url.domain_ = url_decode(rawUrl.c_str() + from, rawUrl.c_str() + rawUrl.size());
            return !url.domain_.empty();
        }

        url.domain_ = url_decode(rawUrl.c_str() + from, rawUrl.c_str() + idx);

        std::string path_and_query = rawUrl.substr(idx);
        if (!decode_url_domain(path_and_query, url.uri_)) {
            return false;
        }

        if (!decode_parameters(path_and_query, url.parameters_)) {
            return false;
        }

        return true;
    }

    bool encode_url(const UrlDetail &url, std::string &result)
    {
        result.reserve(256);
        result += url.protocol_;
        result += "://";
        result += url_encode(url.domain_);

        for (size_t i = 0; i < url.uri_.size(); ++i) {
            result += '/';
            result += url.uri_[i];
        }

        if (!url.parameters_.empty()) {
            result += '?';
            size_t c = 0;
            for (const auto &[key, vals] : url.parameters_) {
                result += url_encode(key);
                result += '=';
                for (size_t v = 0; v < vals.size(); ++v) {
                    result += url_encode(vals[v]);
                    if (v + 1 < vals.size()) {
                        result += '#';  // 多值分隔符（非标准但保持兼容）
                    }
                }
                if (c + 1 < url.parameters_.size()) {
                    result += '&';
                }
                ++c;
            }
        }
        return true;
    }
}
