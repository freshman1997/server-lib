#include "nas/nas_share_manager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#ifdef _WIN32
#include <cwctype>
#include <windows.h>
#endif

namespace yuan::server::nas
{
    namespace
    {
        std::string trim_slashes(std::string text)
        {
            while (!text.empty() && (text.front() == '/' || text.front() == '\\')) {
                text.erase(text.begin());
            }
            while (!text.empty() && (text.back() == '/' || text.back() == '\\')) {
                text.pop_back();
            }
            return text;
        }

        std::filesystem::path path_from_utf8(std::string_view text)
        {
#ifdef _WIN32
            if (text.empty()) {
                return {};
            }
            const int wide_len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (wide_len <= 0) {
                return std::filesystem::path(std::string(text));
            }
            std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), wide_len);
            return std::filesystem::path(wide);
#else
            return std::filesystem::path(std::string(text));
#endif
        }

        std::string path_to_utf8(const std::filesystem::path &path)
        {
#ifdef _WIN32
            const auto wide = path.native();
            if (wide.empty()) {
                return {};
            }
            const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (utf8_len <= 0) {
                return {};
            }
            std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8_len, nullptr, nullptr);
            return utf8;
#else
            return path.string();
#endif
        }
    }

    NasShareManager::NasShareManager(std::vector<NasShare> shares)
        : shares_(std::move(shares))
    {
    }

    NasShareManager::NasShareManager(const NasShareManager &other)
    {
        std::lock_guard<std::mutex> lock(other.mutex_);
        shares_ = other.shares_;
    }

    NasShareManager &NasShareManager::operator=(const NasShareManager &other)
    {
        if (this == &other) {
            return *this;
        }

        std::scoped_lock lock(mutex_, other.mutex_);
        shares_ = other.shares_;
        return *this;
    }

    NasShareManager::NasShareManager(NasShareManager &&other) noexcept
    {
        std::lock_guard<std::mutex> lock(other.mutex_);
        shares_ = std::move(other.shares_);
    }

    NasShareManager &NasShareManager::operator=(NasShareManager &&other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        std::scoped_lock lock(mutex_, other.mutex_);
        shares_ = std::move(other.shares_);
        return *this;
    }

    void NasShareManager::replace(std::vector<NasShare> shares)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shares_ = std::move(shares);
    }

    std::optional<NasShare> NasShareManager::find_by_name(std::string_view name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(shares_.begin(), shares_.end(), [&](const NasShare &share) {
            return share.enabled && share.name == name;
        });
        if (it == shares_.end()) {
            return std::nullopt;
        }
        return *it;
    }

    std::optional<NasResolvedPath> NasShareManager::resolve(std::string_view share_name, std::string_view relative_path) const
    {
        auto share = find_by_name(share_name);
        if (!share) {
            return std::nullopt;
        }

        NasResolvedPath resolved;
        resolved.share = *share;
        resolved.relative_path = normalize_relative_path(relative_path);

        std::filesystem::path root = std::filesystem::absolute(path_from_utf8(share->root_path)).lexically_normal();
        std::filesystem::path full = (root / path_from_utf8(resolved.relative_path)).lexically_normal();

#ifdef _WIN32
        auto lower = [](std::wstring text) {
            for (wchar_t &ch : text) {
                ch = static_cast<wchar_t>(std::towlower(ch));
            }
            return text;
        };
        const auto root_text = root.native();
        const auto full_text = full.native();
        const auto root_cmp = lower(root_text);
        const auto full_cmp = lower(full_text);
        const bool inside = full_cmp == root_cmp || full_cmp.rfind(root_cmp + L"\\", 0) == 0 || full_cmp.rfind(root_cmp + L"/", 0) == 0;
#else
        const std::string root_text = root.string();
        const std::string full_text = full.string();
        const bool inside = full_text == root_text || full_text.rfind(root_text + "/", 0) == 0;
#endif
        if (!inside) {
            return std::nullopt;
        }

        resolved.absolute_path = path_to_utf8(full);
        return resolved;
    }

    std::vector<NasShare> NasShareManager::shares() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return shares_;
    }

    std::string NasShareManager::normalize_relative_path(std::string_view path)
    {
        std::string text = trim_slashes(std::string(path));
        std::filesystem::path normalized = path_from_utf8(text).lexically_normal();
        text = path_to_utf8(normalized);
#ifdef _WIN32
        std::replace(text.begin(), text.end(), '\\', '/');
#endif
        if (text == ".") {
            return {};
        }
        text = trim_slashes(text);
        if (text.rfind("../", 0) == 0 || text == ".." || text.find("/../") != std::string::npos) {
            return "..";
        }
        return text;
    }
}
