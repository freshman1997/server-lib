#include "filesync_scan.h"
#include "filesync_common.h"

#include <cctype>
#include <mutex>
#include <regex>
#include <unordered_map>

namespace filesync::scan {
namespace detail {
struct CacheEntry { std::uintmax_t size; std::uint64_t mtime; std::string hash; };
std::mutex cache_mutex;
std::unordered_map<std::string, CacheEntry> cache;
std::string lower(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
bool internal(const std::filesystem::path& path) {
    const auto name = path.filename().generic_string();
    return name == ".filesync_state.json" || name == ".filesync_state.json.tmp" ||
        name.find(".conflict.") != std::string::npos ||
        name.find(".filesync.tmp.") != std::string::npos || name.find(".filesync.backup.") != std::string::npos;
}
std::string hash(const std::filesystem::path& path, std::uintmax_t size, std::uint64_t mtime) {
    const auto key = path_to_utf8(path);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cache.find(key);
        if (it != cache.end() && it->second.size == size && it->second.mtime == mtime) {
            return it->second.hash;
        }
    }
    const auto value = digest::sha256_file(path);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[key] = {size, mtime, value};
    }
    return value;
}

std::string normalize_pattern(std::string pattern) {
    std::replace(pattern.begin(), pattern.end(), '\\', '/');
    while (pattern.rfind("./", 0) == 0) pattern.erase(0, 2);
    while (!pattern.empty() && pattern.front() == '/') pattern.erase(pattern.begin());
    while (pattern.size() > 1 && pattern.back() == '/') pattern.pop_back();
    return pattern;
}

std::string glob_regex(const std::string& raw) {
    const auto pattern = normalize_pattern(raw);
    std::string result = "^";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '/' && i + 2 < pattern.size() && pattern[i + 1] == '*' && pattern[i + 2] == '*' &&
            i + 3 == pattern.size()) {
            result += "(?:/.*)?";
            i += 2;
            continue;
        }
        if (ch == '*' && i + 2 < pattern.size() && pattern[i + 1] == '*' && pattern[i + 2] == '/') {
            result += "(?:.*/)?";
            i += 2;
            continue;
        }
        if (ch == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
            result += ".*";
            ++i;
        } else if (ch == '*') {
            result += "[^/]*";
        } else if (ch == '?') {
            result += "[^/]";
        } else {
            if (std::string_view(".^$+()[]{}|\\").find(ch) != std::string_view::npos) result.push_back('\\');
            result.push_back(ch);
        }
    }
    result += "$";
    return result;
}

bool matches(const std::string& pattern, const std::string& value) {
    try {
        return std::regex_match(value, std::regex(glob_regex(pattern), std::regex::ECMAScript | std::regex::icase));
    } catch (const std::regex_error&) {
        return false;
    }
}

std::string basename(const std::string& value) {
    const auto slash = value.find_last_of('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

bool pattern_matches_path(const std::string& pattern, const std::string& value) {
    const auto normalized = normalize_pattern(pattern);
    const bool has_slash = normalized.find('/') != std::string::npos;
    auto current = normalize_relative_path(value);
    if (has_slash) {
        const std::string recursive_prefix = "**/";
        const std::string recursive_suffix = "/**";
        if (normalized.rfind(recursive_prefix, 0) == 0 && normalized.size() > recursive_prefix.size() + recursive_suffix.size() &&
            normalized.compare(normalized.size() - recursive_suffix.size(), recursive_suffix.size(), recursive_suffix) == 0) {
            const auto directory_pattern = normalized.substr(recursive_prefix.size(),
                normalized.size() - recursive_prefix.size() - recursive_suffix.size());
            auto slash = current.find('/');
            while (slash != std::string::npos) {
                const auto next = current.find('/', slash + 1);
                const auto component = current.substr(slash + 1, next == std::string::npos ? std::string::npos : next - slash - 1);
                if (next != std::string::npos && matches(directory_pattern, component)) return true;
                slash = next;
            }
            return false;
        }
        return matches(normalized, current);
    }
    while (!current.empty()) {
        if (matches(normalized, basename(current))) return true;
        const auto slash = current.find_last_of('/');
        if (slash == std::string::npos) break;
        current.erase(slash);
    }
    return false;
}

bool any_pattern_matches(const std::vector<std::string>& patterns, const std::string& value) {
    for (const auto& pattern : patterns) {
        if (pattern_matches_path(pattern, value)) return true;
    }
    return false;
}
}

bool included(const model::Config& config, const std::string& remote, bool directory) {
    const auto path = path_from_utf8(remote);
    if (detail::internal(path)) return false;
    if (!directory && !config.include_extensions.empty()) {
        const auto ext = detail::lower(path.extension().generic_string());
        bool allowed = false;
        for (auto value : config.include_extensions) {
            value = detail::lower(value);
            if (!value.empty() && value.front() != '.') value.insert(value.begin(), '.');
            allowed = allowed || ext == value;
        }
        if (!allowed) return false;
    }
    // A directory that does not match an include pattern may still contain
    // matching descendants, so include filters are applied to files only.
    if (!directory && !config.include_patterns.empty() &&
        !detail::any_pattern_matches(config.include_patterns, remote)) return false;
    if (detail::any_pattern_matches(config.exclude_patterns, remote)) return false;
    return true;
}

std::filesystem::path local_path(const model::Config& config, const std::string& remote) {
    if (!is_safe_relative_path(remote)) throw std::runtime_error("unsafe remote path: " + remote);
    for (const auto& entry : config.paths) {
        const auto prefix = normalize_relative_path(entry.remote_prefix);
        if (std::filesystem::is_regular_file(entry.local)) {
            if (remote == (prefix.empty() ? entry.local.filename().generic_string() : prefix)) return entry.local;
        } else if (prefix.empty()) return entry.local / path_from_utf8(remote);
        else if (remote == prefix) return entry.local;
        else if (remote.rfind(prefix + "/", 0) == 0) {
            auto relative = remote.substr(prefix.size() + 1);
            return entry.local / std::filesystem::path(relative);
        }
    }
    throw std::runtime_error("cannot map remote path: " + remote);
}

model::Manifest paths(const model::Config& config) {
    const auto started = std::chrono::steady_clock::now();
    std::cout << "filesync scan start" << std::endl;
    model::Manifest result;
    std::size_t scanned_entries = 0;
    for (const auto& sync : config.paths) {
        if (!std::filesystem::exists(sync.local)) std::filesystem::create_directories(sync.local);
        if (std::filesystem::is_regular_file(sync.local)) {
            const auto remote = sync.remote_prefix.empty() ? sync.local.filename().generic_string() : sync.remote_prefix;
             if (included(config, remote, false)) {
                const auto size = std::filesystem::file_size(sync.local);
                const auto mtime = file_time_to_seconds(std::filesystem::last_write_time(sync.local));
                result[remote] = {false, size, mtime, {}, sync.local};
            }
            continue;
        }
        std::error_code scan_error;
        std::filesystem::recursive_directory_iterator iterator(sync.local, scan_error);
        if (scan_error) throw std::runtime_error("cannot scan path: " + sync.local.string() + ": " + scan_error.message());
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(scan_error)) {
            if (scan_error) throw std::runtime_error("cannot scan path: " + sync.local.string() + ": " + scan_error.message());
            const auto& entry = *iterator;
            ++scanned_entries;
            if (scanned_entries % 10000 == 0) {
                std::cout << "filesync scan progress entries=" << scanned_entries
                          << " files=" << result.size() << std::endl;
            }
            const auto relative = entry.path().lexically_relative(sync.local);
            const auto remote = normalize_relative_path(sync.remote_prefix + (sync.remote_prefix.empty() ? "" : "/") + path_to_utf8(relative));
            if (detail::any_pattern_matches(config.exclude_patterns, remote)) {
                if (entry.is_directory()) iterator.disable_recursion_pending();
                continue;
            }
            if (!included(config, remote, entry.is_directory())) continue;
            if (entry.is_directory()) result[remote] = {true, 0, 0, {}, entry.path()};
            if (entry.is_regular_file()) {
                const auto size = entry.file_size();
                const auto mtime = file_time_to_seconds(entry.last_write_time());
                result[remote] = {false, size, mtime, {}, entry.path()};
            }
        }
        if (scan_error) throw std::runtime_error("cannot scan path: " + sync.local.string() + ": " + scan_error.message());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "filesync scan complete files=" << result.size() << " ms=" << elapsed << std::endl;
    return result;
}
}
