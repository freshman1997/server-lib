#ifndef YUAN_BASE_ALGORITHM_TOP_K_H_
#define YUAN_BASE_ALGORITHM_TOP_K_H_

#include <algorithm>
#include <cstddef>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

namespace yuan::base
{
    // TopK 用固定大小的小根堆保留“最好的 K 个元素”，适合排行榜、搜索结果
    // 截断、统计 topN、监控最大值等场景。
    //
    // Better 表示“lhs 是否比 rhs 更好”。默认 std::greater<int> 会保留最大的 K 个数。
    // 用法：
    //   yuan::base::TopK<int> top(3);
    //   top.push(10); top.push(1); top.push(20);
    //   auto values = top.sorted(); // 按 Better 排序后的结果。
    template <typename T, typename Better = std::greater<T>>
    class TopK
    {
    public:
        explicit TopK(std::size_t k, Better better = Better{})
            : k_(k), better_(std::move(better))
        {
        }

        void push(T value)
        {
            if (k_ == 0) {
                return;
            }

            if (heap_.size() < k_) {
                heap_.push(std::move(value));
                return;
            }

            if (better_(value, heap_.top())) {
                heap_.pop();
                heap_.push(std::move(value));
            }
        }

        std::vector<T> sorted() const
        {
            std::vector<T> values;
            values.reserve(heap_.size());
            auto copy = heap_;
            while (!copy.empty()) {
                values.push_back(copy.top());
                copy.pop();
            }
            std::sort(values.begin(), values.end(), better_);
            return values;
        }

        std::size_t size() const noexcept { return heap_.size(); }
        std::size_t capacity() const noexcept { return k_; }
        bool empty() const noexcept { return heap_.empty(); }

    private:
        struct WorseFirst
        {
            Better better;

            bool operator()(const T &lhs, const T &rhs) const
            {
                return better(lhs, rhs);
            }
        };

        std::size_t k_ = 0;
        Better better_;
        std::priority_queue<T, std::vector<T>, WorseFirst> heap_{WorseFirst{better_}};
    };
}

#endif // YUAN_BASE_ALGORITHM_TOP_K_H_
