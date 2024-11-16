#include "websocket_config.h"
#include "nlohmann/json.hpp"
#include "websocket_utils.h"
#include <ctime>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace net::websocket 
{
    std::string_view heart_beat_interval_key = "heart_beat_interval";
    std::string_view client_key_string_key = "client_key_string";
    std::string_view server_support_protos_key = "server_support_protos";
    std::string_view client_support_protos_key = "client_support_protos";

    class WebSocketConfigManager::ConfigData
    {
    public:
        ConfigData()
        {
            config_file_path_ = "ws_cfg.json";
        }

        bool load_config(bool isServer)
        {
            std::ifstream input(config_file_path_.data());
            try {
                if (!input.good()) {
                    std::cout << "not found config file: " << config_file_path_ << "\n";
                    return false;
                }

                const nlohmann::json &jval = nlohmann::json::parse(input);
                if (jval.is_discarded()) {
                    std::cout << config_file_path_ << " is not json config file!\n";
                    return false;
                }

                config_json_ = jval;

                if (isServer) {
                    const auto &items = config_json_[server_support_protos_key];
                    if (items.is_array()) {
                        for (const auto &item : items.items()) {
                            server_subprotos_.insert(item.value());
                        }
                    }
                } else {
                    const auto &items = config_json_[client_support_protos_key];
                    if (items.is_array()) {
                        for (const auto &item : items.items()) {
                            server_subprotos_.insert(item.value());
                        }
                    }
                }

                return true;
            } catch (...) {
                std::cout << "parse " << config_file_path_ << " config file failed!\n";
                return false;
            }
        }

    public:
        std::string_view config_file_path_;
        nlohmann::json config_json_;
        std::set<std::string> client_subprotos_;
        std::set<std::string> server_subprotos_;
    };

    WebSocketConfigManager::WebSocketConfigManager() : data_(std::make_unique<WebSocketConfigManager::ConfigData>()) {}

    WebSocketConfigManager::~WebSocketConfigManager() {}

    bool WebSocketConfigManager::init(bool isServer)
    {
        return data_->load_config(isServer);
    }

    void WebSocketConfigManager::set_config_path(const std::string_view &path)
    {
        data_->config_file_path_ = path;
    }

    uint32_t WebSocketConfigManager::get_heart_beat_interval() const
    {
        return data_->config_json_.value(heart_beat_interval_key, 0);
    }

    std::string WebSocketConfigManager::get_client_key()
    {
        return data_->config_json_.value(client_key_string_key, WebSocketUtils::gen_magic_string());
    }

    const std::set<std::string> & WebSocketConfigManager::get_client_support_subprotos()
    {
        return data_->client_subprotos_;
    }

    const std::set<std::string> & WebSocketConfigManager::get_server_support_subprotos()
    {
        return data_->server_subprotos_;
    }

    const std::string * WebSocketConfigManager::find_server_support_sub_protocol(const std::set<std::string> &clientProtos)
    {
        for (const auto &proto : clientProtos) {
            auto it = data_->server_subprotos_.find(proto);
            if (it != data_->server_subprotos_.end()) {
                return &*it;
            }
        }
        return nullptr;
    }
}