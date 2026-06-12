#include "base/utils/simple_search.h"

#include <cstdint>
#include <iostream>
#include <set>
#include <string>

namespace
{
    int failed = 0;

    using GuildSearch = yuan::base::SimpleSearch<std::uint64_t>;
    using GuildResults = GuildSearch::ResultSet;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }

    bool has_id(const GuildResults &results, std::uint64_t id)
    {
        for (const auto &result : results) {
            if (result.id == id) {
                return true;
            }
        }
        return false;
    }
}

int main()
{
    GuildSearch guilds;
    guilds.insert(1001, "dragon");
    guilds.insert(1002, "dragonfly");
    guilds.insert(1003, "dragon");
    guilds.insert(1004, "phoenix");
    guilds.insert(1005, "龙之谷");

    check(guilds.size() == 5, "insert should track ids");

    const auto limited = guilds.search("dragon", 2);
    check(limited.size() == 2, "search should stop at requested limit");

    const auto all_dragons = guilds.search("dragon", 0);
    check(all_dragons.size() == 3, "limit zero should search all matches");
    check(has_id(all_dragons, 1001), "search should include first duplicate text id");
    check(has_id(all_dragons, 1002), "search should include prefix extension id");
    check(has_id(all_dragons, 1003), "search should include second duplicate text id");

    const auto prefix_dragons = guilds.prefix_search("drag", 0);
    check(prefix_dragons.size() == 3, "prefix search should collect texts under the prefix node");
    check(!has_id(guilds.prefix_search("gon", 0), 1001), "prefix search should not match middle substrings");

    const auto contains_gon = guilds.contains_search("gon", 0);
    check(contains_gon.size() == 3, "contains search should match middle substrings");
    check(has_id(guilds.search("gon", 0, GuildSearch::Mode::Contains), 1002), "mode dispatch should support contains search");
    check(has_id(guilds.search("drag", 0, GuildSearch::Mode::Prefix), 1001), "mode dispatch should support prefix search");

    guilds.update(1001, "tiger");
    const auto after_update = guilds.search("dragon", 0);
    check(!has_id(after_update, 1001), "update should remove old id from old text");
    check(has_id(after_update, 1003), "update should not remove another id with same text");
    check(has_id(guilds.search("tiger", 0), 1001), "update should index new text");
    check(!has_id(guilds.contains_search("ragon", 0), 1001), "contains index should remove old text on update");
    check(has_id(guilds.contains_search("ige", 0), 1001), "contains index should add new text on update");

    guilds.remove(1003);
    const auto after_remove_id = guilds.search("dragon", 0);
    check(!has_id(after_remove_id, 1003), "remove id should only remove that id");
    check(has_id(after_remove_id, 1002), "remove id should keep related longer text");
    check(!has_id(guilds.contains_search("ragon", 0), 1003), "contains index should remove deleted id");

    guilds.remove_text("dragonfly");
    check(!has_id(guilds.search("dragon", 0), 1002), "remove_text should remove all ids with exact text");
    check(!has_id(guilds.contains_search("fly", 0), 1002), "contains index should remove ids deleted by text");

    check(has_id(guilds.search("龙", 0), 1005), "utf8 search should match Chinese text");
    check(has_id(guilds.contains_search("之", 0), 1005), "contains search should match utf8 byte substrings");

    GuildSearch gap_search;
    gap_search.insert(2001, "axb");
    check(!has_id(gap_search.search("ab", 0), 2001), "gap disabled should not skip unmatched chars");
    gap_search.set_enable_gap(true);
    check(has_id(gap_search.search("ab", 0), 2001), "gap enabled should skip unmatched chars");

    GuildSearch invalid_search;
    std::string invalid_utf8;
    invalid_utf8.push_back(static_cast<char>(0xE4));
    invalid_search.insert(3001, invalid_utf8);
    const auto invalid_results = invalid_search.search(invalid_utf8, 0);
    check(invalid_results.size() == 1, "invalid utf8 should not hang and should support empty-token text");

    yuan::base::SimpleSearch<std::string> string_ids;
    string_ids.insert("guild-alpha", "alpha");
    check(string_ids.search("alpha", 0).begin()->id == "guild-alpha", "search should support string ids");

    GuildSearch large;
    constexpr std::uint64_t large_count = 10000;
    for (std::uint64_t i = 0; i < large_count; ++i) {
        large.insert(i, "guild_" + std::to_string(i) + "_dragon_zone");
    }

    check(large.size() == large_count, "large smoke should insert all ids");
    check(large.prefix_search("guild_99", 20).size() <= 20, "large prefix search should respect limit");
    check(large.contains_search("dragon", 20).size() <= 20, "large contains search should respect limit");
    check(large.search("guild", 20).size() <= 20, "large fuzzy search should respect limit");
    check(!large.prefix_search("guild_9999", 0).empty(), "large prefix search should find exact prefix");
    check(!large.contains_search("9999_dragon", 0).empty(), "large contains search should find indexed substring");

    for (std::uint64_t i = 0; i < 1000; ++i) {
        large.remove(i);
    }
    check(large.size() == large_count - 1000, "large smoke should remove ids");
    check(!has_id(large.contains_search("guild_999_dragon", 0), 999), "large contains index should remove deleted ids");

    if (failed != 0) {
        std::cerr << "simple_search failed=" << failed << '\n';
        return 1;
    }

    std::cout << "simple_search passed\n";
    return 0;
}
