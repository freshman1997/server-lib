#ifndef YUAN_BASE_CONTAINER_BOUNDED_QUEUE_H_
#define YUAN_BASE_CONTAINER_BOUNDED_QUEUE_H_

#include <cstddef>
#include <deque>
#include <utility>

namespace yuan::base
{
    // BoundedQueue 是固定容量队列，容量满时按策略处理溢出。
    //
    // 适用场景：日志积压、审计事件、最近消息、网络发送队列、订阅消息缓冲。
    // 用法：
    //   yuan::base::BoundedQueue<std::string> q(1024);
    //   q.push("event");
    //   std::string out;
    //   q.pop(out);
    template <typename T>
    class BoundedQueue
    {
    public:
        enum class OverflowPolicy
        {
            DropOldest,
            DropNewest,
            Reject,
        };

        explicit BoundedQueue(std::size_t capacity, OverflowPolicy policy = OverflowPolicy::DropOldest)
            : capacity_(capacity), policy_(policy)
        {
        }

        bool push(T value)
        {
            if (capacity_ == 0) {
                ++dropped_;
                return false;
            }

            if (queue_.size() >= capacity_) {
                if (policy_ == OverflowPolicy::Reject || policy_ == OverflowPolicy::DropNewest) {
                    ++dropped_;
                    return false;
                }
                queue_.pop_front();
                ++dropped_;
            }

            queue_.push_back(std::move(value));
            return true;
        }

        bool pop(T &out)
        {
            if (queue_.empty()) {
                return false;
            }
            out = std::move(queue_.front());
            queue_.pop_front();
            return true;
        }

        const T* front() const
        {
            return queue_.empty() ? nullptr : &queue_.front();
        }

        void clear()
        {
            queue_.clear();
        }

        std::size_t size() const noexcept { return queue_.size(); }
        std::size_t capacity() const noexcept { return capacity_; }
        std::size_t dropped() const noexcept { return dropped_; }
        bool empty() const noexcept { return queue_.empty(); }

    private:
        std::size_t capacity_ = 0;
        OverflowPolicy policy_ = OverflowPolicy::DropOldest;
        std::deque<T> queue_;
        std::size_t dropped_ = 0;
    };
}

#endif // YUAN_BASE_CONTAINER_BOUNDED_QUEUE_H_
