#define FILESYNC_PEER_TESTING
#include "filesync_peer.cpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

    void check_throws(const std::function<void()> &fn, const char *message)
    {
        try {
            fn();
            check(false, message);
        } catch (const std::exception &) {
            check(true, message);
        }
    }

    std::filesystem::path make_temp_root()
    {
        const auto base = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int index = 0; index < 100; ++index) {
            auto candidate = base / ("release-filesync-test-" + std::to_string(stamp) + "-" + std::to_string(index));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec)) {
                return candidate;
            }
        }
        throw std::runtime_error("failed to create temp test directory");
    }

    void write_text_file(const std::filesystem::path &path, const std::string &text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    std::string read_text_file(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::vector<char> bytes_of(const std::string &text)
    {
        return std::vector<char>(text.begin(), text.end());
    }

    Config test_config_for(const std::filesystem::path &root)
    {
        Config config;
        config.conflict_strategy = "newer_wins";
        config.sync_deletes = true;
        config.paths.push_back({root, "work"});
        return config;
    }

    void test_token_path_and_hex_helpers()
    {
        const std::string input = "alpha beta/%20?=+";
        const auto encoded = filesync::quote_token(input);
        const auto decoded = filesync::unquote_token(encoded);
        check(decoded == input, "quote_token roundtrip");

        check(!filesync::is_safe_relative_path("../escape"), "reject dotdot path");
        check(!filesync::is_safe_relative_path("work/../escape"), "reject nested dotdot path");
        check(!filesync::is_safe_relative_path("work/./escape"), "reject dot path component");
        check(!filesync::is_safe_relative_path("."), "reject current-directory path");
        check(!filesync::is_safe_relative_path("/absolute"), "reject absolute path");
        check(!filesync::is_safe_relative_path("C:/absolute"), "reject Windows drive path");
        check(!filesync::is_safe_relative_path("a:b"), "reject colon path");
        check(!filesync::is_safe_relative_path("a\\b"), "reject backslash path");
        check(filesync::is_safe_relative_path("work/docs/readme.txt"), "accept relative path");

        const auto normalized = filesync::normalize_relative_path(std::filesystem::path("/work/docs"));
        check(normalized == "work/docs", "normalize_relative_path strips leading slash");

        const std::vector<char> data{'a', '\0', static_cast<char>(0xff)};
        check(filesync::hex_decode(filesync::hex_encode(data)) == data, "hex encode/decode roundtrip");
        check_throws([]() { (void)filesync::hex_decode("abc"); }, "hex_decode rejects odd payload");
        check_throws([]() { (void)filesync::hex_decode("zz"); }, "hex_decode rejects non-hex payload");
    }

    void test_config_rejects_unsafe_remote_prefix()
    {
        const auto root = make_temp_root();
        const auto config_path = root / "config.json";
        write_text_file(config_path,
                        "{\"paths\":[{\"local\":\"" + root.generic_string() +
                            "\",\"remote_prefix\":\"../escape\"}]}");
        check_throws([&]() { (void)load_config(config_path); }, "load_config rejects unsafe remote_prefix");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void test_need_message_counts_delete_and_get_actions()
    {
        const auto root = make_temp_root();
        auto config = test_config_for(root);

        Manifest previous;
        previous["work/old.txt"] = {false, 3, 1, 11};
        save_state(config, previous);

        Manifest remote;
        remote["work/old.txt"] = {false, 3, 1, 11};
        remote["work/new.txt"] = {false, 3, 2, 22};

        const auto need = need_message(config, remote);
        check(need.find("NEED 2\n") == 0, "NEED header counts DELETE and GET actions");
        check(need.find("DELETE work/old.txt\n") != std::string::npos, "NEED includes delete action");
        check(need.find("GET work/new.txt\n") != std::string::npos, "NEED includes get action");

        const auto parsed = parse_need("NEED 3\nGET work/new.txt\nGET ../escape\nGET C:/escape\nEND\n");
        check(parsed.size() == 1 && parsed.count("work/new.txt") == 1, "parse_need ignores unsafe GET paths");

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void test_apply_files_commits_only_verified_payload()
    {
        const auto root = make_temp_root();
        auto config = test_config_for(root);

        const auto expected_path = root / "expected.txt";
        write_text_file(expected_path, "hello");
        const auto expected_hash = filesync::fnv1a_file_hash(expected_path);

        const auto bad_text =
            std::string("FILES 1\nPUT_BEGIN work/a.txt 5 1 ") + std::to_string(expected_hash) + "\n" +
            "CHUNK " + filesync::hex_encode(bytes_of("bogus")) + "\nPUT_END\nEND\n";
        check_throws([&]() { apply_files(config, {}, false, bad_text); },
                     "apply_files rejects hash-mismatched streamed payload");
        check(!std::filesystem::exists(root / "a.txt"), "hash-mismatched payload is not committed");
        check(!std::filesystem::exists(root / "a.txt.filesync.tmp"), "hash-mismatched temp file is removed");

        const auto good_text =
            std::string("FILES 1\nPUT_BEGIN work/a.txt 5 1 ") + std::to_string(expected_hash) + "\n" +
            "CHUNK " + filesync::hex_encode(bytes_of("hello")) + "\nPUT_END\nEND\n";
        apply_files(config, {}, false, good_text);
        check(read_text_file(root / "a.txt") == "hello", "apply_files commits verified streamed payload");

        const auto legacy_bad =
            std::string("FILES 1\nPUT work/b.txt 5 1 ") + std::to_string(expected_hash) + " " +
            filesync::hex_encode(bytes_of("bogus")) + "\nEND\n";
        check_throws([&]() { apply_files(config, {}, false, legacy_bad); },
                     "apply_files rejects hash-mismatched one-line payload");
        check(!std::filesystem::exists(root / "b.txt"), "hash-mismatched one-line payload is not committed");

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
}

int main()
{
    test_token_path_and_hex_helpers();
    test_config_rejects_unsafe_remote_prefix();
    test_need_message_counts_delete_and_get_actions();
    test_apply_files_commits_only_verified_payload();

    return g_failed == 0 ? 0 : 1;
}
