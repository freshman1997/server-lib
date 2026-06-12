#ifndef YUAN_BASE_ID_ID_GENERATOR_H_
#define YUAN_BASE_ID_ID_GENERATOR_H_

#include <atomic>
#include <cstdint>
#include <limits>

namespace yuan::base
{
    // AtomicIdGenerator 是线程安全的递增 ID 生成器，默认从 1 开始并跳过 0。
    //
    // 适用场景：连接 id、请求 id、任务 id、插件资源 id、本进程内唯一句柄。
    // 用法：
    //   yuan::base::AtomicIdGenerator<uint64_t> ids;
    //   auto id = ids.next();
    template <typename T = std::uint64_t>
    class AtomicIdGenerator
    {
    public:
        explicit AtomicIdGenerator(T start = 1)
            : next_(normalize(start))
        {
        }

        T next()
        {
            T value = next_.fetch_add(1, std::memory_order_relaxed);
            while (value == 0) {
                value = next_.fetch_add(1, std::memory_order_relaxed);
            }
            return value;
        }

        void reset(T start = 1)
        {
            next_.store(normalize(start), std::memory_order_relaxed);
        }

    private:
        static T normalize(T value)
        {
            return value == 0 ? 1 : value;
        }

        std::atomic<T> next_;
    };
}

#endif // YUAN_BASE_ID_ID_GENERATOR_H_
