#include "../src/redis_client.h"

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

    RedisClient client({.host_ = "127.0.0.1", .port_ = 6379, .db_ = 1});

    auto res = client.zadd("test_zset", { { "a", 1.0 }, { "b", 2.0 }, { "c", 3.0 } });
    if (res) {
        std::cout << "time: " << res->to_string() << std::endl;
    } else {
        if (auto err = client.get_last_error()) {
            std::cout << "error: " << err->to_string() << std::endl;
        }
    }

    res = client.hmset("test_hash", { { "hello", "world" }, { "hello1", "world1" } });
    if (res) {
        std::cout << "test_hash: " << res->to_string() << std::endl;
    } else {
        if (auto err = client.get_last_error()) {
            std::cout << "error: " << err->to_string() << std::endl;
        }
    }

    res = client.hgetall("test_hash");
    if (res) {
        std::cout << "members: " << res->to_string() << std::endl;
    } else {
        if (auto err = client.get_last_error()) {
            std::cout << "error: " << err->to_string() << std::endl;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}