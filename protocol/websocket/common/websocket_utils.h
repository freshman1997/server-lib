#ifndef __NET_WEBSOCKET_COMMON_WEBSOCKET_UTILS_H__
#define __NET_WEBSOCKET_COMMON_WEBSOCKET_UTILS_H__
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::websocket 
{
    class WebSocketUtils
    {
    public:
        static std::vector<uint8_t> gen_mask_keys();

        static std::string generate_server_key(const std::string &clientKey);

        static const std::string_view & get_magic_string()
        {
            return magic_string_;
        }

        static std::string gen_magic_string();

    private:
        static std::string_view magic_string_;
    };

}

#endif