#include "filesync_common.h"

#include <iostream>
#include <string>

namespace
{
    int g_failed = 0;

    void check(bool condition, const char *message)
    {
        if (!condition) {
            ++g_failed;
            std::cerr << "FAIL: " << message << "\n";
        }
    }
}

int main()
{
    {
        const std::string input = "alpha beta/%20?=+";
        const auto encoded = filesync::quote_token(input);
        const auto decoded = filesync::unquote_token(encoded);
        check(decoded == input, "quote_token roundtrip");
    }

    {
        check(!filesync::is_safe_relative_path("../escape"), "reject dotdot path");
        check(!filesync::is_safe_relative_path("/absolute"), "reject absolute path");
        check(!filesync::is_safe_relative_path("a\\b"), "reject backslash path");
        check(filesync::is_safe_relative_path("work/docs/readme.txt"), "accept relative path");
    }

    {
        const auto normalized = filesync::normalize_relative_path(std::filesystem::path("/work/docs"));
        check(normalized == "work/docs", "normalize_relative_path strips leading slash");
    }

    return g_failed == 0 ? 0 : 1;
}
