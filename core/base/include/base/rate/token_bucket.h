#ifndef YUAN_BASE_RATE_TOKEN_BUCKET_H_
#define YUAN_BASE_RATE_TOKEN_BUCKET_H_

#include <algorithm>
#include <chrono>

namespace yuan::base
{
    // TokenBucket 是令牌桶限流器，按固定速率补充令牌，允许一定突发流量。
    //
    // 适用场景：API 限流、连接限流、登录尝试限流、插件资源消耗控制。
    // 用法：
    //   yuan::base::TokenBucket bucket(100.0, 200.0); // 每秒 100，最多突发 200。
    //   if (bucket.try_consume()) { ... }
    template <typename Clock = std::chrono::steady_clock>
    class TokenBucket
    {
    public:
        using TimePoint = typename Clock::time_point;

        TokenBucket(double rate_per_second, double burst, TimePoint now = Clock::now())
            : rate_per_second_(rate_per_second), burst_(std::max(0.0, burst)), tokens_(std::max(0.0, burst)), last_(now)
        {
        }

        bool try_consume(double tokens = 1.0, TimePoint now = Clock::now())
        {
            refill(now);
            if (tokens_ < tokens) {
                return false;
            }
            tokens_ -= tokens;
            return true;
        }

        double available(TimePoint now = Clock::now())
        {
            refill(now);
            return tokens_;
        }

    private:
        void refill(TimePoint now)
        {
            if (now <= last_) {
                return;
            }
            const auto elapsed = std::chrono::duration<double>(now - last_).count();
            tokens_ = std::min(burst_, tokens_ + elapsed * rate_per_second_);
            last_ = now;
        }

        double rate_per_second_ = 0.0;
        double burst_ = 0.0;
        double tokens_ = 0.0;
        TimePoint last_;
    };
}

#endif // YUAN_BASE_RATE_TOKEN_BUCKET_H_
