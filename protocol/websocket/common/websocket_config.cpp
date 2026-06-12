#include "websocket_config.h"
#include "nlohmann/json.hpp"
#include "websocket_protocol.h"
#include "websocket_utils.h"
#include <ctime>
#include "logger.h"
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace yuan::net::websocket
{
    std::string_view heart_beat_interval_key = "heart_beat_interval";
    std::string_view client_key_string_key = "client_key_string";
    std::string_view server_support_protos_key = "server_support_protos";
    std::string_view client_support_protos_key = "client_support_protos";
    std::string_view server_use_mask_key = "server_use_mask";
    std::string_view client_use_mask_key = "client_use_mask";
    std::string_view heat_beat_timeout_key = "heat_beat_timeout";
    std::string_view outbound_queue_max_messages_key = "outbound_queue_max_messages";
    std::string_view outbound_queue_max_bytes_key = "outbound_queue_max_bytes";
    std::string_view close_handshake_timeout_key = "close_handshake_timeout";
    std::string_view handshake_timeout_key = "handshake_timeout";
    std::string_view read_idle_timeout_key = "read_idle_timeout";
    std::string_view allowed_origins_key = "allowed_origins";
    std::string_view max_message_size_key = "max_message_size";
    std::string_view max_fragmented_message_size_key = "max_fragmented_message_size";
    std::string_view max_connections_per_ip_key = "max_connections_per_ip";
    std::string_view handshake_rate_limit_max_key = "handshake_rate_limit_max";
    std::string_view handshake_rate_limit_window_key = "handshake_rate_limit_window";
    std::string_view message_rate_limit_max_key = "message_rate_limit_max";
    std::string_view message_rate_limit_window_key = "message_rate_limit_window";
    std::string_view tls_cert_path_key = "tls_cert_path";
    std::string_view tls_key_path_key = "tls_key_path";
    constexpr std::string_view websocket_any_origin = "*";

    class WebSocketConfigManager::ConfigData
    {
    public:
        ConfigData()
        {
            config_file_path_ = "ws_cfg.json";
            config_json_ = nlohmann::json::object();
        }

        bool load_config(bool isServer)
        {
            std::ifstream input(config_file_path_.data());
            try
            {
                if (!input.good()) {
                    LOG_WARN("not found config file: {}", config_file_path_);
                    return true;
                }

                const nlohmann::json &jval = nlohmann::json::parse(input);
                if (jval.is_discarded()) {
                    LOG_WARN("{} is not json config file!", config_file_path_);
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

                    const auto &origins = config_json_[allowed_origins_key];
                    if (origins.is_array()) {
                        for (const auto &item : origins.items()) {
                            allowed_origins_.insert(item.value());
                        }
                    }
                } else {
                    const auto &items = config_json_[client_support_protos_key];
                    if (items.is_array()) {
                        for (const auto &item : items.items()) {
                            client_subprotos_.insert(item.value());
                        }
                    }
                }

                return true;
            }
            catch (...)
            {
                LOG_ERROR("parse {} config file failed!", config_file_path_);
                return false;
            }
        }

    public:
        std::string config_file_path_;
        nlohmann::json config_json_;
        std::set<std::string> client_subprotos_;
        std::set<std::string> server_subprotos_;
        std::set<std::string> allowed_origins_;
        std::function<bool(std::string_view)> origin_validator_;
        std::function<bool(const yuan::net::http::HttpRequest &)> auth_validator_;
    };

    WebSocketConfigManager::WebSocketConfigManager()
        : data_(std::make_unique<WebSocketConfigManager::ConfigData>())
    {
    }

    WebSocketConfigManager::~WebSocketConfigManager()
    {
    }

    bool WebSocketConfigManager::init(bool isServer)
    {
        return data_->load_config(isServer);
    }

    void WebSocketConfigManager::set_config_path(const std::string_view & path)
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

    const std::set<std::string> &WebSocketConfigManager::get_client_support_subprotos()
    {
        return data_->client_subprotos_;
    }

    const std::set<std::string> &WebSocketConfigManager::get_server_support_subprotos()
    {
        return data_->server_subprotos_;
    }

    const std::string *WebSocketConfigManager::find_server_support_sub_protocol(const std::set<std::string> & clientProtos)
    {
        for (const auto &proto : clientProtos) {
            auto it = data_->server_subprotos_.find(proto);
            if (it != data_->server_subprotos_.end()) {
                return &*it;
            }
        }
        return nullptr;
    }

    bool WebSocketConfigManager::is_server_use_mask()
    {
        return data_->config_json_.value(server_use_mask_key, 0);
    }

    bool WebSocketConfigManager::is_client_use_mask()
    {
        return data_->config_json_.value(client_use_mask_key, 1);
    }

    uint32_t WebSocketConfigManager::get_heat_beat_timeout()
    {
        return data_->config_json_.value(heat_beat_timeout_key, 0);
    }

    uint32_t WebSocketConfigManager::get_outbound_queue_max_messages() const
    {
        return data_->config_json_.value(outbound_queue_max_messages_key, 0);
    }

    uint32_t WebSocketConfigManager::get_outbound_queue_max_bytes() const
    {
        return data_->config_json_.value(outbound_queue_max_bytes_key, 0);
    }

    uint32_t WebSocketConfigManager::get_close_handshake_timeout() const
    {
        return data_->config_json_.value(close_handshake_timeout_key, DEFAULT_CLOSE_HANDSHAKE_TIMEOUT_MS);
    }

    uint32_t WebSocketConfigManager::get_handshake_timeout() const
    {
        return data_->config_json_.value(handshake_timeout_key, DEFAULT_HANDSHAKE_TIMEOUT_MS);
    }

    uint32_t WebSocketConfigManager::get_read_idle_timeout() const
    {
        return data_->config_json_.value(read_idle_timeout_key, 0);
    }

    uint32_t WebSocketConfigManager::get_max_message_size() const
    {
        return data_->config_json_.value(max_message_size_key, PACKET_MAX_BYTE);
    }

    uint32_t WebSocketConfigManager::get_max_fragmented_message_size() const
    {
        return data_->config_json_.value(max_fragmented_message_size_key, get_max_message_size());
    }

    uint32_t WebSocketConfigManager::get_max_connections_per_ip() const
    {
        return data_->config_json_.value(max_connections_per_ip_key, 0);
    }

    uint32_t WebSocketConfigManager::get_handshake_rate_limit_max() const
    {
        return data_->config_json_.value(handshake_rate_limit_max_key, 0);
    }

    uint32_t WebSocketConfigManager::get_handshake_rate_limit_window() const
    {
        return data_->config_json_.value(handshake_rate_limit_window_key, DEFAULT_RATE_LIMIT_WINDOW_MS);
    }

    uint32_t WebSocketConfigManager::get_message_rate_limit_max() const
    {
        return data_->config_json_.value(message_rate_limit_max_key, 0);
    }

    uint32_t WebSocketConfigManager::get_message_rate_limit_window() const
    {
        return data_->config_json_.value(message_rate_limit_window_key, DEFAULT_RATE_LIMIT_WINDOW_MS);
    }

    std::string WebSocketConfigManager::get_tls_cert_path() const
    {
        return data_->config_json_.value(tls_cert_path_key, std::string(DEFAULT_TLS_CERT_PATH));
    }

    std::string WebSocketConfigManager::get_tls_key_path() const
    {
        return data_->config_json_.value(tls_key_path_key, std::string(DEFAULT_TLS_KEY_PATH));
    }

    bool WebSocketConfigManager::is_origin_allowed(std::string_view origin) const
    {
        if (origin.empty()) {
            return true;
        }

        if (data_->origin_validator_) {
            return data_->origin_validator_(origin);
        }

        if (data_->allowed_origins_.empty()) {
            return true;
        }

        return data_->allowed_origins_.find(std::string(websocket_any_origin)) != data_->allowed_origins_.end() ||
               data_->allowed_origins_.find(std::string(origin)) != data_->allowed_origins_.end();
    }

    void WebSocketConfigManager::set_origin_validator(std::function<bool(std::string_view)> validator)
    {
        data_->origin_validator_ = std::move(validator);
    }

    bool WebSocketConfigManager::is_request_authorized(const yuan::net::http::HttpRequest &request) const
    {
        if (!data_->auth_validator_) {
            return true;
        }
        return data_->auth_validator_(request);
    }

    void WebSocketConfigManager::set_auth_validator(std::function<bool(const yuan::net::http::HttpRequest &)> validator)
    {
        data_->auth_validator_ = std::move(validator);
    }

    void WebSocketConfigManager::force_client_mask()
    {
        data_->config_json_[client_use_mask_key] = 1;
    }
}
