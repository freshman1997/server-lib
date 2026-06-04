#include "utils.h"

#include <iostream>
#include <sstream>
#include <cstdint>
#include <iomanip>
#include <cmath>

namespace yuan::redis
{
    std::string serializeDouble(double value) 
    {
        if (std::isnan(value)) {
            return "nan";
        }
        
        if (std::isinf(value)) {
            return value < 0 ? "-inf" : "inf";
        }

        std::ostringstream oss;
        
        // 检查是否为整数
        if (value >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            value <= static_cast<double>(std::numeric_limits<int64_t>::max()) &&
            value == static_cast<int64_t>(value)) {
            oss << static_cast<int64_t>(value);
        } else {
            // 对于浮点数，使用足够精度
            oss << std::setprecision(15) << value;
            
            std::string result = oss.str();
            // 移除尾随的零
            size_t dot_pos = result.find('.');
            if (dot_pos != std::string::npos) {
                result = result.erase(result.find_last_not_of('0') + 1);
                if (result.back() == '.') {
                    result.pop_back();
                }
            }
            return result;
        }
        
        return oss.str();
    }

    // 特殊值映射
    const std::unordered_map<std::string, double> RedisDoubleConverter::SPECIAL_VALUES = {
        {"nan", std::numeric_limits<double>::quiet_NaN()},
        {"inf", std::numeric_limits<double>::infinity()},
        {"+inf", std::numeric_limits<double>::infinity()},
        {"-inf", -std::numeric_limits<double>::infinity()},
        {"infinity", std::numeric_limits<double>::infinity()},
        {"+infinity", std::numeric_limits<double>::infinity()},
        {"-infinity", -std::numeric_limits<double>::infinity()}
    };
}
