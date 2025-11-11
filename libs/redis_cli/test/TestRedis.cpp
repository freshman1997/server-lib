#include "redis_cli_manager.h"
#include "redis_value.h"
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <iostream>

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif
    using namespace yuan::redis;
    RedisCliManager::get_instance()->init({
        { .db_ = 1, .name_ = "redis1"},
    });

    auto client = RedisCliManager::get_instance()->get_round_robin_redis_client();
    auto res = client->info();
    if (res) {
        std::cout << "push: " << res->to_string() << std::endl;
    } else {
        if (auto err = client->get_last_error()) {
            std::cout << "error: " << err->to_string() << std::endl;
        }
    }

    int i = 0;
    while (client->is_connected())
    {
        res = client->publish("test1", "helloworld" + std::to_string(i + 1));
        if (res) {
            std::cout << "push: " << res->to_string() << std::endl;
        } else {
            if (auto err = client->get_last_error()) {
                std::cout << "error: " << err->to_string() << std::endl;
            }
        }

        res = client->publish("test2", "你好世界" + std::to_string(i + 1));
        if (res) {
            std::cout << "push: " << res->to_string() << std::endl;
        } else {
            if (auto err = client->get_last_error()) {
                std::cout << "error: " << err->to_string() << std::endl;
            }
        }

        ++i;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}