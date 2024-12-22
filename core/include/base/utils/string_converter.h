//
// Created by jackeyuan on 2024/5/26.
//

#ifndef STRING_CONVERTER_H
#define STRING_CONVERTER_H
#include <string>

namespace yuan::base::encoding
{
    std::string GBKToUTF8(const std::string &strGBK);
    std::string UTF8ToGBK(const std::string &strUTF8);
}

#endif //STRING_CONVERTER_H
