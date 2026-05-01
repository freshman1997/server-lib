#include "magnet_uri.h"
#include "utils.h"
#include <algorithm>
#include <sstream>

namespace yuan::net::bit_torrent
{

    static std::string url_decode(const std::string &str)
    {
        std::string result;
        result.reserve(str.size());
        for (size_t i = 0; i < str.size(); ++i)
        {
            if (str[i] == '%' && i + 2 < str.size())
            {
                char hex[3] = {str[i + 1], str[i + 2], '\0'};
                char *end = nullptr;
                long val = std::strtol(hex, &end, 16);
                if (end && *end == '\0' && val >= 0 && val <= 255)
                {
                    result += static_cast<char>(val);
                    i += 2;
                    continue;
                }
            }
            else if (str[i] == '+')
            {
                result += ' ';
                continue;
            }
            result += str[i];
        }
        return result;
    }

    static std::vector<uint8_t> base32_decode(const std::string &input)
    {
        static const int8_t kDecodeTable[256] = {
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
            15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
            -1, 26, 27, 28, 29, 30, 31, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        };

        std::vector<uint8_t> output;
        output.reserve(input.size() * 5 / 8);

        int buffer = 0;
        int bits_left = 0;
        for (char ch : input)
        {
            if (ch == '=' || ch == ' ')
                continue;
            int8_t val = kDecodeTable[static_cast<uint8_t>(ch)];
            if (val < 0)
                continue;
            buffer = (buffer << 5) | val;
            bits_left += 5;
            if (bits_left >= 8)
            {
                bits_left -= 8;
                output.push_back(static_cast<uint8_t>((buffer >> bits_left) & 0xFF));
            }
        }
        return output;
    }

    MagnetUri MagnetUri::parse(const std::string &uri)
    {
        MagnetUri result;

        if (uri.size() < 8 || uri.substr(0, 8) != "magnet:?")
            return result;

        std::string params = uri.substr(8);

        std::istringstream ss(params);
        std::string token;
        while (std::getline(ss, token, '&'))
        {
            auto eq = token.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = token.substr(0, eq);
            std::string value = url_decode(token.substr(eq + 1));

            if (key == "xt")
            {
                if (value.find("urn:btih:") == 0)
                {
                    std::string hash_str = value.substr(9);
                    if (hash_str.size() == 40)
                    {
                        result.info_hash = from_hex(hash_str);
                        result.info_hash_hex = hash_str;
                    }
                    else if (hash_str.size() == 32)
                    {
                        result.info_hash = base32_decode(hash_str);
                        result.info_hash_hex = to_hex(result.info_hash);
                    }
                }
            }
            else if (key == "dn")
            {
                result.display_name = value;
            }
            else if (key == "tr")
            {
                result.tracker_urls.push_back(value);
            }
        }

        if (result.info_hash.size() == 20)
        {
            result.valid = true;
        }

        return result;
    }

} // namespace yuan::net::bit_torrent
