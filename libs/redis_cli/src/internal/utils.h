#ifndef __YUNA_REDIS_UTILS_H__
#define __YUNA_REDIS_UTILS_H__
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <limits>
#include <cctype>
#include <charconv>
#include <system_error>

namespace yuan::redis 
{
    extern std::string serializeDouble(double value);

    class FastDoubleConverter 
    {
    public:
        static double convertFromChars(const std::string& str) {
            double result;
            auto [ptr, ec] = std::from_chars(
                str.data(), 
                str.data() + str.length(), 
                result
            );
            
            if (ec == std::errc::invalid_argument) {
                throw std::invalid_argument("Not a valid number: " + str);
            } else if (ec == std::errc::result_out_of_range) {
                throw std::out_of_range("Number out of range: " + str);
            } else if (ptr != str.data() + str.length()) {
                throw std::invalid_argument("Invalid characters in number: " + str);
            }
            
            return result;
        }
        
        // 带格式控制的版本
        static double convertFromCharsFormat(const std::string& str, std::chars_format fmt = std::chars_format::general) 
        {
            double result;
            auto [ptr, ec] = std::from_chars(
                str.data(), 
                str.data() + str.length(), 
                result,
                fmt
            );
            
            if (ec != std::errc()) {
                handleError(ec, str);
            }
            
            return result;
        }
        
    private:
        static void handleError(std::errc ec, const std::string& str) 
        {
            switch (ec) {
                case std::errc::invalid_argument:
                    throw std::invalid_argument("Not a valid number: " + str);
                case std::errc::result_out_of_range:
                    throw std::out_of_range("Number out of range: " + str);
                default:
                    throw std::runtime_error("Conversion error: " + str);
            }
        }
    };

    class RedisDoubleConverter 
    {
    private:
        static const std::unordered_map<std::string, double> SPECIAL_VALUES;
        
    public:
        static double convert(const std::string& str) {
            // 检查空字符串
            if (str.empty()) {
                throw std::invalid_argument("Empty string cannot be converted to double");
            }
            
            // 转换为小写以便比较
            std::string lower_str = toLower(str);
            
            // 检查特殊值
            auto it = SPECIAL_VALUES.find(lower_str);
            if (it != SPECIAL_VALUES.end()) {
                return it->second;
            }
            
            // 检查infinity表示法
            if (lower_str == "+inf" || lower_str == "inf") {
                return std::numeric_limits<double>::infinity();
            }
            if (lower_str == "-inf") {
                return -std::numeric_limits<double>::infinity();
            }
            
            // 使用快速转换
            return FastDoubleConverter::convertFromChars(str);
        }
        
        // 安全转换，提供默认值
        static double convertSafe(const std::string& str, double default_value = 0.0) {
            try {
                return convert(str);
            } catch (const std::exception& e) {
                return default_value;
            }
        }
        
        // 批量转换
        static std::vector<double> convertBatch(const std::vector<std::string>& strings) {
            std::vector<double> results;
            results.reserve(strings.size());
            
            for (const auto& str : strings) {
                results.push_back(convert(str));
            }
            
            return results;
        }
        
    private:
        static std::string toLower(const std::string& str) {
            std::string result = str;
            std::transform(result.begin(), result.end(), result.begin(),
                        [](unsigned char c) { return std::tolower(c); });
            return result;
        }
    };
}

#endif // __YUNA_REDIS_UTILS_H__