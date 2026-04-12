#ifndef __LOG_SINGLETON_H__
#define __LOG_SINGLETON_H__

#include <memory>
#include <mutex>

#ifdef __unix__
#include <pthread.h>
#endif

namespace yuan::log
{
namespace singleton
{

template <typename T, typename... Args>
class Singleton
{
public:
    Singleton() = default;
    ~Singleton() = default;
    explicit Singleton(const Singleton<T>&) = delete;
    Singleton& operator=(const Singleton<T>&) = delete;

    static std::shared_ptr<T> _instance;

    static std::shared_ptr<T> get_instance(Args... args)
    {
        static std::once_flag s_flag;
        std::call_once(s_flag, [&] {
#ifdef __unix__
            // 强制引用符号，避免某些静态链接场景下 pthread 被错误优化掉。
            void* pc = reinterpret_cast<void*>(&pthread_create);
            (void)pc;
#endif
            _instance = std::make_shared<T>(args...);
        });
        return _instance;
    }
};

template <typename T, typename... Args>
std::shared_ptr<T> Singleton<T, Args...>::_instance = nullptr;

} // namespace singleton

template <typename T, typename... Args>
using Singleton = singleton::Singleton<T, Args...>;

} // namespace yuan::log

#endif
