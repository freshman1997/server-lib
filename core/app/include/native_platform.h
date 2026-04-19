#ifndef YUAN_APP_NATIVE_PLATFORM_H
#define YUAN_APP_NATIVE_PLATFORM_H

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace yuan::app
{
    class NativePlatformGuard
    {
    public:
        NativePlatformGuard()
        {
#ifdef _WIN32
            WSADATA wsa{};
            initialized_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
            initialized_ = true;
#endif
        }

        ~NativePlatformGuard()
        {
#ifdef _WIN32
            if (initialized_) {
                WSACleanup();
            }
#endif
        }

        bool ok() const noexcept
        {
            return initialized_;
        }

    private:
        bool initialized_ = false;
    };
}

#endif
