#include "websocket_utils.h"
#include "base/time.h"
#include "base/utils/base64.h"
#include "base/utils/string_util.h"
#include <openssl/sha.h>
#include <string>

namespace net::websocket 
{
    std::vector<uint8_t> WebSocketUtils::gen_mask_keys()
    {
        std::vector<uint8_t> res;
        // TODO 产生掩码

        return res;
    }

    std::string WebSocketUtils::generate_server_key(const std::string &clientKey)
    {
        std::string magic = clientKey + magic_string_.data();
        unsigned char *hash = SHA1((unsigned char *)magic.c_str(), magic.size(), nullptr);
        return base::util::base64_encode((const char *)hash);
    }

    std::string WebSocketUtils::gen_magic_string()
    {
        uint32_t now = base::time::now();
        srand(now);
        const std::string &res = base::util::to_hex_string(rand() ^ 0x12fdefacd) + "-" + base::util::to_hex_string(rand() ^ 0x12fdeface) + "-" + base::util::to_hex_string(rand() ^ 0x12fdefaef) + "-" + base::util::to_hex_string(rand());
        return base::util::base64_encode(res);
    }

    std::string_view WebSocketUtils::magic_string_ = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}