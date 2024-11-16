#ifndef __NET_WEBSOCKET_COMMON_WEBSOCKET_CONFIG_H__
#define __NET_WEBSOCKET_COMMON_WEBSOCKET_CONFIG_H__
#include "singleton/singleton.h"
#include <memory>
#include <set>

namespace net::websocket 
{
    // config keys
    extern std::string_view heart_beat_interval_key;
    extern std::string_view client_key_string_key;
    extern std::string_view server_support_protos_key;
    extern std::string_view client_support_protos_key;

    class WebSocketConfigManager : public singleton::Singleton<WebSocketConfigManager>
    {
    public:
        WebSocketConfigManager();
        ~WebSocketConfigManager();

        bool init(bool isServer);

        // default ws_cfg.json
        void set_config_path(const std::string_view &path);

        uint32_t get_heart_beat_interval() const;

        std::string get_client_key();

        const std::set<std::string> & get_client_support_subprotos();

        const std::set<std::string> & get_server_support_subprotos();

        const std::string * find_server_support_sub_protocol(const std::set<std::string> &clientProtos);
        
    private:
        class ConfigData;
        std::unique_ptr<ConfigData> data_;
    };
}

#endif