#include "webdav_lock_manager.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <sstream>

namespace yuan::net::webdav
{
    namespace
    {
        std::string normalize_href(std::string_view href)
        {
            std::string out(href);
            if (out.empty() || out.front() != '/') {
                out.insert(out.begin(), '/');
            }
            while (out.size() > 1 && out.back() == '/') {
                out.pop_back();
            }
            return out;
        }

        std::string make_token()
        {
            static std::random_device rd;
            static std::mt19937_64 rng(rd());
            std::uniform_int_distribution<unsigned long long> dist;
            std::ostringstream oss;
            oss << "opaquelocktoken:" << std::hex << dist(rng) << dist(rng);
            return oss.str();
        }
    }

    LockInfo WebDavLockManager::create(std::string href, LockScope scope, Depth depth, std::string owner,
                                       std::chrono::seconds timeout)
    {
        prune_expired();
        LockInfo info;
        info.token = make_token();
        info.href = normalize_href(href);
        info.scope = scope;
        info.depth = depth;
        info.owner = std::move(owner);
        info.expires_at = std::chrono::steady_clock::now() + timeout;
        locks_[info.token] = info;
        return info;
    }

    bool WebDavLockManager::refresh(std::string_view token, std::chrono::seconds timeout)
    {
        prune_expired();
        auto it = locks_.find(normalize_token(token));
        if (it == locks_.end()) {
            return false;
        }
        it->second.expires_at = std::chrono::steady_clock::now() + timeout;
        return true;
    }

    bool WebDavLockManager::unlock(std::string_view token)
    {
        prune_expired();
        return locks_.erase(normalize_token(token)) > 0;
    }

    bool WebDavLockManager::allows(std::string_view href, std::string_view if_header_or_token) const
    {
        prune_expired();
        const std::string token = normalize_token(if_header_or_token);
        for (const auto &[_, lock] : locks_) {
            if (!covers(lock, href)) {
                continue;
            }
            if (!token.empty() && token == lock.token) {
                continue;
            }
            return false;
        }
        return true;
    }

    std::vector<LockInfo> WebDavLockManager::active_locks(std::string_view href) const
    {
        prune_expired();
        std::vector<LockInfo> out;
        for (const auto &[_, lock] : locks_) {
            if (covers(lock, href)) {
                out.push_back(lock);
            }
        }
        return out;
    }

    std::optional<LockInfo> WebDavLockManager::find(std::string_view token) const
    {
        prune_expired();
        auto it = locks_.find(normalize_token(token));
        if (it == locks_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void WebDavLockManager::prune_expired() const
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = locks_.begin(); it != locks_.end();) {
            if (it->second.expires_at <= now) {
                it = locks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool WebDavLockManager::covers(const LockInfo &lock, std::string_view href)
    {
        const std::string target = normalize_href(href);
        if (target == lock.href) {
            return true;
        }
        if (lock.depth != Depth::infinity) {
            return false;
        }
        const std::string prefix = lock.href == "/" ? "/" : lock.href + "/";
        return target.rfind(prefix, 0) == 0;
    }

    std::string WebDavLockManager::normalize_token(std::string_view token)
    {
        std::string text(token);
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        };
        trim(text);
        const std::string marker = "opaquelocktoken:";
        const auto pos = text.find(marker);
        if (pos != std::string::npos) {
            text = text.substr(pos);
            const auto end = text.find_first_of(">) \t\r\n");
            if (end != std::string::npos) {
                text.resize(end);
            }
        }
        trim(text);
        if (text.size() >= 2 && text.front() == '<' && text.back() == '>') {
            text = text.substr(1, text.size() - 2);
        }
        return text;
    }
}
