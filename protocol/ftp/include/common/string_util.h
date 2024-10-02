#ifndef _NET_FTP_COMMON_STRING_UTIL_H__
#define _NET_FTP_COMMON_STRING_UTIL_H__
#include <cctype>

namespace net::ftp 
{
    inline bool start_with_ignore_case(const char *begin, const char *end, const char *str)
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

    static int end_with(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p) {
            if (*p == *p1) {
                ++p1; ++p;
                for (; *p1 && *p == *p1 && p != end; ) {
                    ++p1; ++p;
                }
                if (p == end && !(*p1)) {
                    return true;
                }
            }
        }
        return -1;
    }

    inline int find_first(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p) {
            for (; *p1 && *p == *p1 && p < end; ++p1, ++p);
        }
        return !(*p1) ? (p - begin)  - (p1 - str) - 1: -1;
    }
}

#endif