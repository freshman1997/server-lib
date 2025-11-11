#include "redis_cli_manager.h"
#include "redis_value.h"
#include "value/array_value.h"
#include "value/int_value.h"
#include "value/string_value.h"
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

    auto subcribeClient = RedisCliManager::get_instance()->get_round_robin_redis_client();
    std::cout << "subcribeClient: " << subcribeClient->get_name() << std::endl;

    auto subRes = subcribeClient->psubscribe({"test*"}, [](const auto &msgs) {
        for (const auto& msg : msgs) {
            std::cout << "pattern:" << msg.pattern->to_string() << ", channel: " << msg.channel->to_string() << ", value: " << msg.message->to_string() << std::endl;
        }
    });

    if (subRes) {
        std::cout << "subRes: " << subRes->to_string() << std::endl;
    } else {
        if (auto err = subcribeClient->get_last_error()) {
            std::cout << "error: " << err->to_string() << std::endl;
        }
    }

    while (subcribeClient->is_connected() && !subcribeClient->is_closed())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (subcribeClient->is_subcribing()) {
            int ret = subcribeClient->receive(2000);
            if (ret < 0) {
                if (subcribeClient->get_last_error()) {
                    std::cout << "receive error: " << subcribeClient->get_last_error()->to_string() << std::endl;
                } else {
                    std::cout << "receive error: " << ret << std::endl;
                }
                subcribeClient->close();
            }
        }

        auto res = subcribeClient->punsubscribe({"test*"});
        if (res) {
            std::cout << "punsubscribe: " << res->to_string() << std::endl;
        } else {
            if (auto err = subcribeClient->get_last_error()) {
                std::cout << "punsubscribe error: " << err->to_string() << std::endl;
            }
        }

        res = subcribeClient->set("hello1", "world1");
        if (res) {
            std::cout << "set: " << res->to_string() << std::endl;
        } else {
            if (auto err = subcribeClient->get_last_error()) {
                std::cout << "set error: " << err->to_string() << std::endl;
            }
        }

        res = subcribeClient->get("hello1");
        if (res) {
            std::cout << "get: " << res->to_string() << std::endl;
        } else {
            if (auto err = subcribeClient->get_last_error()) {
                std::cout << "get error: " << err->to_string() << std::endl;
            }
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}