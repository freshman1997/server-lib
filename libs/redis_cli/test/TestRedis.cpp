#include "../src/redis_client.h"
#include "../src/cmd/string_cmd.h"
#include "../src/internal/redis_registry.h"
#include "event/event_loop.h"

#include <Windows.h>
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

    auto strCmd = std::make_shared<StringCmd>();
    strCmd->set_args("get", {std::make_shared<StringValue>("hello")});
    strCmd->set_callback([&client](std::shared_ptr<RedisValue> value){
        if (value) {
            std::cout << "callback get value: " << value->to_string() << std::endl;
        } else {
            if (client.get_last_error()) {
                std::cout << client.get_last_error()->to_string() << std::endl;
            }
        }
        RedisRegistry::get_instance()->get_event_loop()->quit();
    }); 

    client.execute_command(strCmd);

    RedisRegistry::get_instance()->get_event_loop()->loop();


#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}