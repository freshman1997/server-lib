#ifndef YUAN_BASE_CONTAINER_RECENT_SET_H_
#define YUAN_BASE_CONTAINER_RECENT_SET_H_

#include <cstddef>
#include <deque>
#include <unordered_set>
#include <utility>

namespace yuan::base
{
    // RecentSet 记录最近出现过的 N 个值，用于快速判断是否重复；超过容量后最旧值
    // 会被移除。
    //
    // 适用场景：消息去重、请求 id/nonce 防重放、最近处理项过滤。
    // 用法：
    //   yuan::base::RecentSet<uint64_t> seen(1024);
    //   bool duplicated = seen.seen_or_add(id);
    template <typename T, typename Hash = std::hash<T>, typename Equal = std::equal_to<T>>
    class RecentSet
    {
    public:
        explicit RecentSet(std::size_t capacity)
            : capacity_(capacity)
        {
        }

        bool seen_or_add(T value)
        {
            if (capacity_ == 0) {
                return false;
            }

            if (set_.find(value) != set_.end()) {
                return true;
            }

            order_.push_back(value);
            set_.insert(std::move(value));
            while (order_.size() > capacity_) {
                set_.erase(order_.front());
                order_.pop_front();
            }
            return false;
        }

        bool contains(const T &value) const
        {
            return set_.find(value) != set_.end();
        }

        void clear()
        {
            order_.clear();
            set_.clear();
        }

        std::size_t size() const noexcept { return set_.size(); }
        std::size_t capacity() const noexcept { return capacity_; }
        bool empty() const noexcept { return set_.empty(); }

    private:
        std::size_t capacity_ = 0;
        std::deque<T> order_;
        std::unordered_set<T, Hash, Equal> set_;
    };
}

#endif // YUAN_BASE_CONTAINER_RECENT_SET_H_
