#include "nas/nas_share_manager.h"

#include <algorithm>
#include <filesystem>

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

        std::filesystem::path root = std::filesystem::absolute(std::filesystem::path(share->root_path)).lexically_normal();
        std::filesystem::path full = (root / std::filesystem::path(resolved.relative_path)).lexically_normal();
        const std::string root_text = root.string();
        const std::string full_text = full.string();

#ifdef _WIN32
        auto lower = [](std::string text) {
            for (char &ch : text) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return text;
        };
        const std::string root_cmp = lower(root_text);
        const std::string full_cmp = lower(full_text);
        const bool inside = full_cmp == root_cmp || full_cmp.rfind(root_cmp + "\\", 0) == 0 || full_cmp.rfind(root_cmp + "/", 0) == 0;
#else
        const bool inside = full_text == root_text || full_text.rfind(root_text + "/", 0) == 0;
#endif
        if (!inside) {
            return std::nullopt;
        }

        resolved.absolute_path = full_text;
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
        std::filesystem::path normalized = std::filesystem::path(text).lexically_normal();
        text = normalized.generic_string();
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
