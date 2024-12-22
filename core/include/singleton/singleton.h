#ifndef __SINGLETON_H__
#define __SINGLETON_H__
#include <memory>
#include <mutex>

namespace yuan::singleton 
{
    using namespace std;
    template <typename T>
    class Singleton {
    protected:
        Singleton() = default;
        Singleton(const Singleton<T>&) = delete;
        Singleton& operator=(const Singleton<T>& st) = delete;

        static std::shared_ptr<T> _instance;

    public:
        static std::shared_ptr<T> get_instance() {
            static std::once_flag s_flag;
            std::call_once(s_flag, [&]() {
                _instance = shared_ptr<T>(new T);
            });
            return _instance;
        }


        ~Singleton() = default;
    };

    template <typename T>
    std::shared_ptr<T> Singleton<T>::_instance = nullptr;
}

#endif
