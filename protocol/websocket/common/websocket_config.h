#ifndef __NET_WEBSOCKET_COMMON_WEBSOCKET_CONFIG_H__
#define __NET_WEBSOCKET_COMMON_WEBSOCKET_CONFIG_H__
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <functional>

namespace yuan::net::http
{
    class HttpRequest;
}

namespace yuan::net::websocket
{
    extern std::string_view heart_beat_interval_key;
    extern std::string_view client_key_string_key;
    extern std::string_view server_support_protos_key;
    extern std::string_view client_support_protos_key;
    extern std::string_view server_use_mask_key;
    extern std::string_view client_use_mask_key;
    extern std::string_view heat_beat_timeout_key;
    extern std::string_view outbound_queue_max_messages_key;
    extern std::string_view outbound_queue_max_bytes_key;
    extern std::string_view close_handshake_timeout_key;
    extern std::string_view handshake_timeout_key;
    extern std::string_view read_idle_timeout_key;
    extern std::string_view allowed_origins_key;
    extern std::string_view max_message_size_key;
    extern std::string_view max_fragmented_message_size_key;
    extern std::string_view max_connections_per_ip_key;
    extern std::string_view handshake_rate_limit_max_key;
    extern std::string_view handshake_rate_limit_window_key;
    extern std::string_view message_rate_limit_max_key;
    extern std::string_view message_rate_limit_window_key;
    extern std::string_view tls_cert_path_key;
    extern std::string_view tls_key_path_key;

    class WebSocketConfigManager
    {
    public:
        WebSocketConfigManager();
        ~WebSocketConfigManager();

        bool init(bool isServer);

        void set_config_path(const std::string_view &path);

        uint32_t get_heart_beat_interval() const;

        std::string get_client_key();

        const std::set<std::string> &get_client_support_subprotos();

        const std::set<std::string> &get_server_support_subprotos();

        const std::string *find_server_support_sub_protocol(const std::set<std::string> &clientProtos);

        bool is_server_use_mask();

        bool is_client_use_mask();

        uint32_t get_heat_beat_timeout();

        uint32_t get_outbound_queue_max_messages() const;

        uint32_t get_outbound_queue_max_bytes() const;

        uint32_t get_close_handshake_timeout() const;

        uint32_t get_handshake_timeout() const;

        uint32_t get_read_idle_timeout() const;

        uint32_t get_max_message_size() const;

        uint32_t get_max_fragmented_message_size() const;

        uint32_t get_max_connections_per_ip() const;

        uint32_t get_handshake_rate_limit_max() const;

        uint32_t get_handshake_rate_limit_window() const;

        uint32_t get_message_rate_limit_max() const;

        uint32_t get_message_rate_limit_window() const;

        std::string get_tls_cert_path() const;

        std::string get_tls_key_path() const;

        bool is_origin_allowed(std::string_view origin) const;

        void set_origin_validator(std::function<bool(std::string_view)> validator);

        bool is_request_authorized(const yuan::net::http::HttpRequest &request) const;

        void set_auth_validator(std::function<bool(const yuan::net::http::HttpRequest &)> validator);

        void force_client_mask();

    private:
        class ConfigData;
        std::unique_ptr<ConfigData> data_;
    };
}

#endif
