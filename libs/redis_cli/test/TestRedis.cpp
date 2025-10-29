#include "../src/redis_client.h"
#include "../src/cmd/string_cmd.h"
#include "../src/internal/redis_registry.h"
#include "cmd/default_cmd.h"
#include "event/event_loop.h"

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

    RedisClient client;
    int ret = client.connect("127.0.0.1", 6379);
    if (ret != 0)
    {
        std::cout << "connect redis failed" << std::endl;
        return -1;
    }

    auto strCmd1 = std::make_shared<DefaultCmd>();
    strCmd1->set_args("set", {std::make_shared<StringValue>("hello world")});
    auto res1 = client.execute_command(strCmd1).get_result();

    if (res1) {
        std::cout << "set result: " << res1->to_string() << std::endl;
    } else {
        if (client.get_last_error()) {
            std::cout << client.get_last_error()->to_string() << std::endl;
        }
    }

    auto strCmd = std::make_shared<StringCmd>();
    strCmd->set_args("get", {std::make_shared<StringValue>("hello")});

    auto res = client.execute_command(strCmd).get_result();
    if (res) {
        std::cout << "get value: " << res->to_string() << std::endl;
    } else {
        if (client.get_last_error()) {
            std::cout << client.get_last_error()->to_string() << std::endl;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}