#ifndef __CONFIG_LOADER_H__
#define __CONFIG_LOADER_H__
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace net::http 
{
    class HttpConfigManager
    {
    public:
        HttpConfigManager();

    public:
        bool good();

        bool reload_config();

    public:
        uint32_t get_uint_property(const std::string &key);
        int32_t get_int_property(const std::string &key);
        bool get_bool_property(const std::string &key);
        double get_double_property(const std::string &key);
        std::string get_string_property(const std::string &key);

        template<typename T>
        std::vector<T> get_type_array_properties(const std::string &key)
        {
            const auto &val = config_json_[key];
            if (!val.is_array()) {
                return std::vector<T>();
            }

            std::vector<T> res;
            for (const auto &item : val) {
                res.emplace_back(item);
            }

            return res;
        }

    private:
        bool load_config();

    private:
        bool is_good_;
        nlohmann::json config_json_;
    };
}

#endif