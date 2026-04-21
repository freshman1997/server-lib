#ifndef __YUAN_NET_AUTH_RATE_LIMITER_H__
#define __YUAN_NET_AUTH_RATE_LIMITER_H__

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace yuan::net
{
    struct AuthRateLimiterConfig
    {
        int max_failures = 5;
        int window_seconds = 60;
        int ban_seconds = 300;
    };

    class AuthRateLimiter
    {
    public:
        explicit AuthRateLimiter(AuthRateLimiterConfig config = {})
            : config_(std::move(config))
        {
        }

        bool is_banned(const std::string &client_ip)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            auto it = bans_.find(client_ip);
            if (it != bans_.end()) {
                if (now < it->second) {
                    return true;
                }
                bans_.erase(it);
            }
            return false;
        }

        void record_failure(const std::string &client_ip)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            auto &entry = entries_[client_ip];
            const auto cutoff = now - std::chrono::seconds(config_.window_seconds);
            while (!entry.timestamps.empty() && entry.timestamps.front() < cutoff) {
                entry.timestamps.pop_front();
            }
            entry.timestamps.push_back(now);
            if (static_cast<int>(entry.timestamps.size()) >= config_.max_failures) {
                bans_[client_ip] = now + std::chrono::seconds(config_.ban_seconds);
                entry.timestamps.clear();
            }
        }

        void record_success(const std::string &client_ip)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.erase(client_ip);
        }

    private:
        struct FailureEntry
        {
            std::deque<std::chrono::steady_clock::time_point> timestamps;
        };

        AuthRateLimiterConfig config_;
        std::mutex mutex_;
        std::unordered_map<std::string, FailureEntry> entries_;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> bans_;
    };
}

#endif
