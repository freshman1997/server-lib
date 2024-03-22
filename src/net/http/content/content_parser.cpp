#include <cctype>
#include <string>
#include <unordered_map>
#include "net/http/content/content_parser.h"

namespace net::http 
{
    static bool str_cmp(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p, ++p1) {
            if (std::tolower(*p) != *p1) {
                return false;
            }
        }

        return p == end && !(*p1);
    }

    static std::pair<bool, std::string> read_identifier(const char *p, const char *end)
    {
        std::string id;
        bool quoted = false;
        bool ended = false;
        for (; p != end; ++p) {
            char ch = *p;
            if (ch == ' ') {
                continue;
            }

            if (ch == '\"') {
                if (quoted) break;
                quoted = true;
                continue;
            }

            if (ch == '=' || ch == ';' || ch == '\n') {
                break;
            }

            id.push_back(ch);
        }

        return {p == end, id};
    }

    ContentDispistion ContentParser::parse_content_disposition(const char *begin, const char *end)
    {
        ContentDispistion res = {0, {}};
        const char *p = begin;
        if (p + 21 > end) {
            return res;
        }

        if (!str_cmp(p, p + 20, "content-disposition:")) {
            return res;
        }

        p += 20;

        std::string type;
        for (; p <= end; ++p) {
            char ch = *p;
            if (ch == ' ') {
                continue;
            }

            if (ch == ';') {
                ++p;
                break;
            }
            type.push_back(ch);
        }

        std::unordered_map<std::string, std::string> &kvs = res.second.second;
        while (p <= end) {
            const auto &k = read_identifier(p, end);
            if (k.second.empty()) {
                return res;
            }

            p += k.second.size() + 2;
            const auto &v = read_identifier(p, end);
            if (v.second.empty()) {
                return res;
            }

            kvs[k.second] = v.second;
            p += v.second.size() + 2;
            if (v.first) {
                break;
            }

            if (*p == ';') {
                ++p;
            }

            
        }

        if (kvs.empty()) {
            return res;
        }

        res.first = p - begin;
        res.second.first = type;

        return res;
    }
}