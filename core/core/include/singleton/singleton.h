#ifndef __SINGLETON_H__
#define __SINGLETON_H__
#include <memory>
#include <mutex>

#ifdef __unix__
#include <pthread.h>
#endif

namespace yuan::singleton 
{
    using namespace std;
    template <typename T, typename ...Args>
    class Singleton {
    protected:
        Singleton() = default;
        Singleton(const Singleton<T>&) = delete;
        Singleton& operator=(const Singleton<T>& st) = delete;

        static std::shared_ptr<T> _instance;

    public:
        static std::shared_ptr<T> get_instance(Args ...args) {
            static std::once_flag s_flag;
            std::call_once(s_flag, [&]() {
            
            // 强制引用符号，防止被编译器优化掉
            #ifdef __unix__
                void *pc = (void *) &pthread_create;
            #endif

                _instance = shared_ptr<T>(new T(args...));
            }, args...);
            return _instance;
        }


        ~Singleton() = default;
    };

    template <typename T, typename ...Args>
    std::shared_ptr<T> Singleton<T, Args...>::_instance = nullptr;
}

#endif
