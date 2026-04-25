#include "base/utils/base64.h"

#include <iostream>
#include <string>
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
    const std::vector<std::string> cases{
        "",
        "f",
        "fo",
        "foo",
        "bob:reader",
        "alice:secret",
    };

    for (const auto &text : cases) {
        const auto encoded = yuan::base::util::base64_encode(text);
        const auto decoded = yuan::base::util::base64_decode(encoded);
        check(decoded == text, "base64 roundtrip should preserve input");
    }

    check(yuan::base::util::base64_decode("Zg==") == "f", "base64 should decode two padding chars");
    check(yuan::base::util::base64_decode("Zm8=") == "fo", "base64 should decode one padding char");
    check(yuan::base::util::base64_decode("Zm9v") == "foo", "base64 should decode no padding chars");

    if (failed != 0) {
        std::cerr << "base64 failed=" << failed << '\n';
        return 1;
    }
    std::cout << "base64 passed\n";
    return 0;
}
