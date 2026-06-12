#ifndef YUAN_BASE_CONTAINER_RING_BUFFER_H_
#define YUAN_BASE_CONTAINER_RING_BUFFER_H_

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace yuan::base
{
    // RingBuffer 是固定容量环形缓冲区，写满后新数据覆盖最旧数据。
    //
    // 适用场景：最近 N 条事件、指标采样窗口、包历史、调试记录。
    // 用法：
    //   yuan::base::RingBuffer<int> rb(3);
    //   rb.push_back(1); rb.push_back(2); rb.push_back(3); rb.push_back(4);
    //   rb[0]; // 现在是 2。
    template <typename T>
    class RingBuffer
    {
    public:
        explicit RingBuffer(std::size_t capacity)
            : values_(capacity)
        {
        }

        void push_back(T value)
        {
            if (values_.empty()) {
                ++dropped_;
                return;
            }

            const std::size_t index = (start_ + size_) % values_.size();
            if (size_ == values_.size()) {
                values_[start_] = std::move(value);
                start_ = (start_ + 1) % values_.size();
                ++dropped_;
                return;
            }

            values_[index] = std::move(value);
            ++size_;
        }

        bool pop_front(T &out)
        {
            if (size_ == 0) {
                return false;
            }
            out = std::move(values_[start_]);
            start_ = (start_ + 1) % values_.size();
            --size_;
            return true;
        }

        const T& operator[](std::size_t index) const
        {
            if (index >= size_) {
                throw std::out_of_range("RingBuffer index out of range");
            }
            return values_[(start_ + index) % values_.size()];
        }

        void clear() noexcept
        {
            start_ = 0;
            size_ = 0;
        }

        std::size_t size() const noexcept { return size_; }
        std::size_t capacity() const noexcept { return values_.size(); }
        std::size_t dropped() const noexcept { return dropped_; }
        bool empty() const noexcept { return size_ == 0; }
        bool full() const noexcept { return size_ == values_.size() && !values_.empty(); }

    private:
        std::vector<T> values_;
        std::size_t start_ = 0;
        std::size_t size_ = 0;
        std::size_t dropped_ = 0;
    };
}

#endif // YUAN_BASE_CONTAINER_RING_BUFFER_H_
