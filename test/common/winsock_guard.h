#ifndef TEST_COMMON_WINSOCK_GUARD_H
#define TEST_COMMON_WINSOCK_GUARD_H

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace test::common
{
    class WinsockGuard
    {
    public:
        WinsockGuard()
        {
#ifdef _WIN32
            WSADATA wsa{};
            initialized_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
            initialized_ = true;
#endif
        }

        ~WinsockGuard()
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
