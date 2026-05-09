#include "webdav_lock_manager.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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
            static std::atomic<uint64_t> seq{ 1 };
            const auto id = seq.fetch_add(1, std::memory_order_relaxed);
            const auto now = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            std::ostringstream oss;
            oss << "opaquelocktoken:" << std::hex << now << id;
            return oss.str();
        }
    }

    LockInfo WebDavLockManager::create(std::string href, LockScope scope, Depth depth, std::string owner,
                                       std::chrono::seconds timeout)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired_locked(now);
        LockInfo info;
        info.token = make_token();
        info.href = normalize_href(href);
        info.scope = scope;
        info.depth = depth;
        info.owner = std::move(owner);
        info.expires_at = now + timeout;
        locks_[info.token] = info;
        return info;
    }

    bool WebDavLockManager::refresh(std::string_view token, std::chrono::seconds timeout)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired_locked(now);
        auto it = locks_.find(normalize_token(token));
        if (it == locks_.end()) {
            return false;
        }
        it->second.expires_at = now + timeout;
        return true;
    }

    bool WebDavLockManager::unlock(std::string_view token)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired_locked(std::chrono::steady_clock::now());
        return locks_.erase(normalize_token(token)) > 0;
    }

    bool WebDavLockManager::allows(std::string_view href, std::string_view if_header_or_token) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired_locked(std::chrono::steady_clock::now());
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
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired_locked(std::chrono::steady_clock::now());
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
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired_locked(std::chrono::steady_clock::now());
        auto it = locks_.find(normalize_token(token));
        if (it == locks_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void WebDavLockManager::prune_expired() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prune_expired_locked(std::chrono::steady_clock::now());
    }

    void WebDavLockManager::prune_expired_locked(std::chrono::steady_clock::time_point now) const
    {
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
