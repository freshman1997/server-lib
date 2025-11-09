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
        { .db_ = 1, .name_ = "redis2"},
    });

    auto client = RedisCliManager::get_instance()->get_round_robin_redis_client();
    std::cout << "client: " << client->get_name() << std::endl;
    auto res = client->publish("test1", "helloworld");
    if (res) {
        std::cout << "push: " << res->to_string() << std::endl;
    } else {
        if (auto err = client->get_last_error()) {
            std::cout << "error: " << err->to_string() << std::endl;
        }
    }

    int i = 0;
    auto subcribeClient = RedisCliManager::get_instance()->get_round_robin_redis_client();
    std::cout << "client: " << subcribeClient->get_name() << std::endl;

    auto subRes = subcribeClient->subscribe({"test1", "test2"}, [](const std::unordered_map<std::string, std::shared_ptr<RedisValue>> &msg) {
        for (auto &[key, value] : msg) {
            std::cout << "channel: " << key << ", value: " << value->to_string() << std::endl;
        }
    });

    if (subRes) {
        std::cout << "subRes: " << subRes->to_string() << std::endl;
    } else {
        if (auto err = subcribeClient->get_last_error()) {
            std::cout << "error: " << err->to_string() << std::endl;
        }
    }

    while (client->is_connected() && subcribeClient->is_connected())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

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

        if (i == 5) {
            res = subcribeClient->unsubscribe({"test1", "test2"});
            if (res) {
                std::cout << "get: " << res->to_string() << std::endl;
                auto arr = res->as<ArrayValue>();
                for (int i = 0; i < arr->get_values().size(); i += 3) {
                    auto tag = arr->get_values()[i]->as<StringValue>();
                    if (tag && tag->get_value() == "unsubscribe") {
                        if (i + 2 >= arr->get_values().size()) {
                            std::cout << "unsubscribe error" << std::endl;
                            break;
                        }

                        auto channel = arr->get_values()[i + 1]->as<StringValue>();
                        auto unsubcribeRes = arr->get_values()[i + 2]->as<IntValue>();

                        if (channel && unsubcribeRes && !channel->get_value().empty() && unsubcribeRes->get_value() == 1) {
                            std::cout << "unsubscribe success: " << channel->get_value() << std::endl;
                            subcribeClient->unsubscibe_channel(channel->get_value());
                        }
                    }
                }
            } else {
                if (auto err = subcribeClient->get_last_error()) {
                    std::cout << "error: " << err->to_string() << std::endl;
                }
            }

            res = subcribeClient->set("test-key1", "实打实大苏打");
            if (res) {
                std::cout << "get: " << res->to_string() << std::endl;
            } else {
                if (auto err = subcribeClient->get_last_error()) {
                    std::cout << "error: " << err->to_string() << std::endl;
                }
            }

            res = subcribeClient->get("test-key1");
            if (res) {
                std::cout << "get: " << res->to_string() << std::endl;
            } else {
                if (auto err = subcribeClient->get_last_error()) {
                    std::cout << "error: " << err->to_string() << std::endl;
                }
            }
        }

        if (i == 10) {
            subRes = subcribeClient->subscribe({"test1", "test2"}, [](const std::unordered_map<std::string, std::shared_ptr<RedisValue>> &msg) {
                for (auto &[key, value] : msg) {
                    std::cout << "channel: " << key << ", value: " << value->to_string() << std::endl;
                }
            });

            if (subRes) {
                std::cout << "subRes: " << subRes->to_string() << std::endl;
            } else {
                if (auto err = subcribeClient->get_last_error()) {
                    std::cout << "error: " << err->to_string() << std::endl;
                }
            }
        }
    }

    RedisCliManager::get_instance()->release_all();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}