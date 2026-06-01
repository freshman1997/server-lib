#include "redis_client_pool.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <iostream>

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa); iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif
    using namespace yuan::redis;
    RedisClientPool pool;
    Option option;
    option.port_ = 6378;
    option.db_ = 1;
    option.name_ = "redis1";
    if (!pool.init(option, 1)) {
        std::cout << "connect redis failed" << std::endl;
        return 0;
    }

    auto subcribeClient = pool.get_round_robin_client();
    std::cout << "subcribeClient: " << subcribeClient->get_name() << std::endl;

    auto subRes = subcribeClient->psubscribe({ "test*" }, [](const auto &msgs) {
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

    while (subcribeClient->is_connected()) {
        if (subcribeClient->is_subscribing()) {
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
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
