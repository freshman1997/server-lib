#include "base/utils/compressed_trie.h"

#include <iostream>
#include <string>

namespace
{
    int g_passed = 0;
    int g_failed = 0;

    void check(bool condition, const std::string &message)
    {
        if (condition) {
            ++g_passed;
        } else {
            std::cerr << "  FAIL: " << message << "\n";
            ++g_failed;
        }
    }

    void test_exact_match_requires_terminal()
    {
        yuan::base::CompressTrie trie;
        trie.insert("/api/user");

        check(trie.contains("/api/user"), "inserted key should be contained");
        check(!trie.contains("/api"), "structural split prefix should not count as contained");

        trie.insert("/api/order");
        check(!trie.contains("/api"), "common split node should still not be terminal");
        check(trie.size() == 2, "size should count distinct inserted keys");

        trie.insert("/api/user");
        check(trie.size() == 2, "duplicate insert should not increase size");
    }

    void test_exact_terminal_beats_parent_prefix()
    {
        yuan::base::CompressTrie trie;
        trie.insert("/api", true);
        trie.insert("/api/user");

        const auto exact = trie.find_prefix("/api/user");
        check(exact.match_length == 9, "exact terminal should return full match length");
        check(exact.is_terminal, "exact terminal should be marked terminal");
        check(!exact.is_registered, "non-prefix exact handler should not be reported as registered prefix");

        const auto child = trie.find_prefix("/api/user/42");
        check(child.match_length == 4, "non-prefix child should fall back to parent registered prefix");
        check(child.is_registered, "fallback should be a registered prefix");
        check(!child.is_terminal, "fallback prefix for longer input is not an exact terminal");
    }

    void test_longest_registered_prefix()
    {
        yuan::base::CompressTrie trie;
        trie.insert("/static", true);
        trie.insert("/static/assets", true);

        const auto result = trie.find_prefix("/static/assets/logo.png");
        check(result.match_length == 14, "longest registered prefix should win");
        check(result.is_registered, "longest registered prefix should be marked registered");
        check(!result.is_terminal, "longer input should not be exact terminal");
    }

    void test_key_prefix_lookup()
    {
        yuan::base::CompressTrie trie;
        trie.insert("/static/assets", true);

        check(trie.has_key_with_prefix("/sta"), "key should exist with partial edge prefix");
        check(trie.has_key_with_prefix("/static"), "key should exist with full node prefix");
        check(!trie.has_key_with_prefix("/static/files"), "missing longer prefix should not match");
        check(!trie.has_key_with_prefix("/missing"), "missing prefix should not match");
    }
}

int main()
{
    test_exact_match_requires_terminal();
    test_exact_terminal_beats_parent_prefix();
    test_longest_registered_prefix();
    test_key_prefix_lookup();

    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }
    std::cout << "All compressed trie tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
