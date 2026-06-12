#ifndef YUAN_BASE_RATE_SLIDING_WINDOW_RATE_LIMITER_H_
#define YUAN_BASE_RATE_SLIDING_WINDOW_RATE_LIMITER_H_

#include <chrono>
#include <cstddef>
#include <deque>
#include <unordered_map>

namespace yuan::base
{
    // SlidingWindowRateLimiter 记录每个 key 在时间窗口内的事件次数，超过上限则拒绝。
    //
    // 适用场景：登录失败限制、接口频率限制、同 IP/用户/资源的滑动窗口限流。
    // 用法：
    //   yuan::base::SlidingWindowRateLimiter<std::string> limiter(5, std::chrono::seconds(60));
    //   if (limiter.allow("client-ip")) { ... }
    template <typename Key, typename Clock = std::chrono::steady_clock,
              typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
    class SlidingWindowRateLimiter
    {
    public:
        using Duration = typename Clock::duration;
        using TimePoint = typename Clock::time_point;

        SlidingWindowRateLimiter(std::size_t max_events, Duration window)
            : max_events_(max_events), window_(window)
        {
        }

        bool allow(const Key &key, TimePoint now = Clock::now())
        {
            auto &events = events_[key];
            prune(events, now);
            if (events.size() >= max_events_) {
                return false;
            }
            events.push_back(now);
            return true;
        }

        std::size_t count(const Key &key, TimePoint now = Clock::now())
        {
            auto it = events_.find(key);
            if (it == events_.end()) {
                return 0;
            }
            prune(it->second, now);
            return it->second.size();
        }

        void clear(const Key &key)
        {
            events_.erase(key);
        }

        void clear()
        {
            events_.clear();
        }

    private:
        void prune(std::deque<TimePoint> &events, TimePoint now)
        {
            const auto cutoff = now - window_;
            while (!events.empty() && events.front() <= cutoff) {
                events.pop_front();
            }
        }

        std::size_t max_events_ = 0;
        Duration window_{};
        std::unordered_map<Key, std::deque<TimePoint>, Hash, Equal> events_;
    };
}

#endif // YUAN_BASE_RATE_SLIDING_WINDOW_RATE_LIMITER_H_
