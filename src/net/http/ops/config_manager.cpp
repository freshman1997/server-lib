#include "net/http/ops/config_manager.h"
#include "net/http/ops/option.h"
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

    uint32_t HttpConfigManager::get_uint_property(const std::string &key)
    {
        const auto &item = config_json_[key];
        if (item.is_number_unsigned() || item.is_number_integer()) {
            return item;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atoi(sval.c_str());
        }

        return 0;
    }

    int32_t HttpConfigManager::get_int_property(const std::string &key)
    {
        const auto &item = config_json_[key];
        if (item.is_number_integer()) {
            return item;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atoi(sval.c_str());
        }

        return 0;
    }

    bool HttpConfigManager::get_bool_property(const std::string &key)
    {
        const auto &item = config_json_[key];
        if (item.is_number_integer()) {
            return item == 1;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atoi(sval.c_str()) == 1;
        }

        return 0;
    }

    double HttpConfigManager::get_double_property(const std::string &key)
    {
        const auto &item = config_json_[key];
        if (item.is_number_float()) {
            return item;
        }

        if (item.is_string()) {
            const std::string &sval = item;
            return std::atof(sval.c_str());
        }

        return 0;
    }

    std::string HttpConfigManager::get_string_property(const std::string &key)
    {
        return config_json_[key];
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
                std::cout << "no " << config::config_file_name << " config file found in cwd!!!\n";
                return false;
            }

            const nlohmann::json &jval = nlohmann::json::parse(input);
            if (jval.is_discarded()) {
                std::cout << config::config_file_name << " not a json config file!!!\n";
                return false;
            }

            config_json_ = jval;

            return true;
        } catch (...) {
            std::cout << "parse " << config::config_file_name << " config file failed!!!\n";
            return false;
        }
    }
}