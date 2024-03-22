#include <unordered_map>
#include <vector>

#include "base/utils/base64.h"

namespace base::util
{
    static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string base64_encode(const std::string& data) {
        std::string encoded_data;
        int i = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];

        for (const auto& byte : data) {
            char_array_3[i++] = byte;
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                for (i = 0; i < 4; i++) {
                    encoded_data.push_back(base64_chars[char_array_4[i]]);
                }
                i = 0;
            }
        }

        if (i > 0) {
            int j = i;
            for (; j < 3; j++) {
                char_array_3[j] = '\0';
            }

            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

            for (j = 0; j < i + 1; j++) {
                encoded_data.push_back(base64_chars[char_array_4[j]]);
            }

            while (i++ < 3) {
                encoded_data.push_back('=');
            }
        }

        return encoded_data;
    }

    std::string base64_decode(const std::string& data)
    {
        std::string decoded_data;
        int i = 0;
        unsigned char char_array_4[4];
        for (const auto& byte : data) {
            if (byte == '=') {
                break; // 遇到填充字符'='，结束解码
            }

            if (base64_chars.find(byte) == std::string::npos) {
                continue; // 非Base64字符，忽略
            }
            
            char_array_4[i++] = byte;
            if (i == 4) {
                for (i = 0; i < 4; i++) {
                    char_array_4[i] = base64_chars.find(char_array_4[i]);
                }

                decoded_data.push_back((char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4));
                decoded_data.push_back(((char_array_4[1] & 0x0f) << 4) + ((char_array_4[2] & 0x3c) >> 2));
                decoded_data.push_back(((char_array_4[2] & 0x03) << 6) + char_array_4[3]);
                
                i = 0;
            }
        }

        return decoded_data;
    }
}