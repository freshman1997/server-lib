#ifndef YUAN_BASE_ALGORITHM_WILDCARD_MATCH_H_
#define YUAN_BASE_ALGORITHM_WILDCARD_MATCH_H_

#include <cctype>
#include <string_view>

namespace yuan::base
{
    // wildcard_match 提供简单通配符匹配，支持：
    // - '*' 匹配任意长度字符串
    // - '?' 匹配任意单个字符
    //
    // 适用场景：host 规则、路径规则、配置项过滤、简单名称匹配。
    // 用法：
    //   wildcard_match("*.example.com", "api.example.com");
    //   wildcard_match_ascii_ci("API-*", "api-test");
    inline char ascii_lower(char ch)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    inline bool wildcard_match(std::string_view pattern, std::string_view text, bool ignore_case = false)
    {
        std::size_t p = 0;
        std::size_t t = 0;
        std::size_t star = std::string_view::npos;
        std::size_t star_text = 0;

        auto same = [ignore_case](char lhs, char rhs) {
            if (ignore_case) {
                return ascii_lower(lhs) == ascii_lower(rhs);
            }
            return lhs == rhs;
        };

        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], text[t]))) {
                ++p;
                ++t;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                star_text = t;
            } else if (star != std::string_view::npos) {
                p = star + 1;
                t = ++star_text;
            } else {
                return false;
            }
        }

        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }

        return p == pattern.size();
    }

    inline bool wildcard_match_ascii_ci(std::string_view pattern, std::string_view text)
    {
        return wildcard_match(pattern, text, true);
    }
}

#endif // YUAN_BASE_ALGORITHM_WILDCARD_MATCH_H_
