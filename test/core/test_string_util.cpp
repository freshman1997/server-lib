#include "base/utils/string_util.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    int failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }
}

int main()
{
    using namespace yuan::base::util;

    check(trim_ascii(" \t hello \r\n") == "hello", "trim_ascii should remove ascii spaces");
    check(trim_ascii_copy("  value  ") == "value", "trim_ascii_copy should return string");

    check(lower_ascii("HeLLo-123") == "hello-123", "lower_ascii should lower ascii letters");
    check(upper_ascii("HeLLo-123") == "HELLO-123", "upper_ascii should upper ascii letters");

    std::string inplace = "AbC";
    lower_ascii_inplace(inplace);
    check(inplace == "abc", "lower_ascii_inplace should mutate string");
    upper_ascii_inplace(inplace);
    check(inplace == "ABC", "upper_ascii_inplace should mutate string");

    check(iequals_ascii("Content-Type", "content-type"), "iequals_ascii should ignore ascii case");
    check(!iequals_ascii("abc", "abcd"), "iequals_ascii should compare length");

    check(starts_with("abcdef", "abc"), "starts_with should match prefix");
    check(ends_with("abcdef", "def"), "ends_with should match suffix");
    check(starts_with_ascii_ci("Content-Type", "content"), "starts_with_ascii_ci should ignore case");
    check(ends_with_ascii_ci("hello.JSON", ".json"), "ends_with_ascii_ci should ignore case");
    check(contains_ascii_ci("Cache-Control", "control"), "contains_ascii_ci should ignore case");
    check(!contains_ascii_ci("Cache-Control", "cookie"), "contains_ascii_ci should reject miss");

    const auto views = split_view("a,,b", ',');
    check(views.size() == 3 && views[1].empty(), "split_view should keep empty parts by default");
    const auto compact_views = split_view("a,,b", ',', false);
    check(compact_views.size() == 2 && compact_views[1] == "b", "split_view should optionally drop empty parts");

    const auto strings = split("x:y:z", ':');
    check(strings.size() == 3 && strings[2] == "z", "split should return strings");
    check(join(std::vector<std::string>{"a", "b", "c"}, ",") == "a,b,c", "join should concatenate strings");

    const std::vector<std::uint8_t> bytes{0x00, 0x0f, 0xff};
    check(to_hex(bytes) == "000fff", "to_hex should encode bytes");
    const auto decoded = from_hex("000fff");
    check(decoded == bytes, "from_hex should decode bytes");

    if (failed != 0) {
        std::cerr << "string_util failed=" << failed << '\n';
        return 1;
    }

    std::cout << "string_util passed\n";
    return 0;
}
