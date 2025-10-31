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

    RedisClient client;
    int ret = client.connect("127.0.0.1", 6379);
    if (ret != 0)
    {
        std::cout << "connect redis failed" << std::endl;
        return -1;
    }

    //auto res = client.eval(R"(
    //    local key = KEYS[1]
	//	local value = tonumber(ARGV[1])
	//	local old = tonumber(redis.call("GET", key))
	//	if old == nil then
	//		old = 0
	//	end
	//	redis.call("SET", key, value + old)
	//	return value
    //)", {"key"}, {"1"});
//
    //if (res) {
    //    std::cout << res->to_string() << std::endl;
    //} else {
    //    std::cout << "eval failed" << std::endl;
    //    if (auto err = client.get_last_error()){
    //        std::cout << err->to_string() << std::endl;
    //    }
    //}

    auto res = client.script_load(R"(
        local key = KEYS[1]
		local value = tonumber(ARGV[1])
		local old = tonumber(redis.call("GET", key))
		if old == nil then
			old = 0
		end
		redis.call("SET", key, value + old)
		return value
   )");

    if (res) {
        std::cout << res->to_string() << std::endl;
    } else {
        std::cout << "script_load failed" << std::endl;
        if (auto err = client.get_last_error()){
            std::cout << err->to_string() << std::endl;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}