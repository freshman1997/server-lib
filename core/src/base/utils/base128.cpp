#include "base/utils/base128.h"

namespace yuan::base::util 
{
    std::string base128_encode(uint32_t number)
    {
        std::string encoded_data;
        while (number > 0) {
            unsigned char byte = number & 0x7F; // 取最低7位
            number >>= 7; // 右移7位
            if (number > 0) {
                byte |= 0x80; // 设置最高位为1表示后续还有字节
            }
            encoded_data += byte;
        }
        return encoded_data;
    }

    uint32_t base128_decode(const std::string& data)
    {
        unsigned int number = 0;
        for (const auto& byte : data) {
            number <<= 7; // 左移7位
            number |= (byte & 0x7F); // 取最低7位
            if ((byte & 0x80) == 0) {
                break; // 最高位为0表示结束
            }
        }
        return number;
    }
}