#include "ops/config_manager.h"
#include "ops/option.h"
#include <fstream>
#include <iostream>

namespace net::http 
{
    HttpConfigManager::HttpConfigManager()
    {
        is_good_ = load_config();
    }

    bool HttpConfigManager::good()
    {
        return is_good_;
    }

    uint32_t HttpConfigManager::get_uint_property(const std::string &key, uint32_t defVal)
    {
        const auto &item = config_json_[key];
        if (item.is_number_unsigned() || item.is_number_integer()) {
            return item;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atoi(sval.c_str());
        }

        return defVal;
    }

    int32_t HttpConfigManager::get_int_property(const std::string &key, int defVal)
    {
        const auto &item = config_json_[key];
        if (item.is_number_integer()) {
            return item;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atoi(sval.c_str());
        }

        return defVal;
    }

    bool HttpConfigManager::get_bool_property(const std::string &key, bool defVal)
    {
        const auto &item = config_json_[key];
        if (item.is_number_integer()) {
            return item == 1;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atoi(sval.c_str()) == 1;
        }

        return defVal;
    }

    double HttpConfigManager::get_double_property(const std::string &key, double defVal)
    {
        const auto &item = config_json_[key];
        if (item.is_number_float()) {
            return item;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atof(sval.c_str());
        }

        return defVal;
    }

    std::string HttpConfigManager::get_string_property(const std::string &key, const std::string &defVal)
    {
        const auto &item = config_json_[key];
        if (item.is_string()) {
            return item;
        }

        return defVal;
    }
    
    bool HttpConfigManager::reload_config()
    {
        is_good_ = load_config();
        return is_good_;
    }

    bool HttpConfigManager::load_config()
    {
        std::ifstream input(config::config_file_name);
        try {
            if (!input.good()) {
                std::cout << "no `" << config::config_file_name << "` configuration file found in cwd!\n";
                return false;
            }

            const nlohmann::json &jval = nlohmann::json::parse(input);
            if (jval.is_discarded()) {
                std::cout << config::config_file_name << " not a json config file!\n";
                return false;
            }

            config_json_ = jval;

            return true;
        } catch (...) {
            std::cout << "parse " << config::config_file_name << " config file failed!\n";
            return false;
        }
    }
}