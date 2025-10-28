#include "../src/redis_client.h"
#include "../src/cmd/string_cmd.h"
#include "../src/internal/redis_registry.h"
#include "event/event_loop.h"

#include <iostream>

int main()
{
    using namespace yuan::redis;

    RedisClient client;
    int ret = client.connect("127.0.0.1", 6379);
    if (ret != 0)
    {
        std::cout << "connect redis failed" << std::endl;
        return -1;
    }

    auto strCmd = std::make_shared<StringCmd>();
    strCmd->set_args("get", {std::make_shared<StringValue>("key")});
    strCmd->set_callback([](std::shared_ptr<RedisValue> value){
        if (value)
        {
            std::cout << "callback get value: " << value->to_string() << std::endl;
            RedisRegistry::get_instance()->get_event_loop()->quit();
        }
    }); 

    client.execute_command(strCmd);

    RedisRegistry::get_instance()->get_event_loop()->loop();

    return 0;
}