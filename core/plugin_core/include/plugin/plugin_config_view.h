#ifndef __YUAN_PLUGIN_PLUGIN_CONFIG_VIEW_H__
#define __YUAN_PLUGIN_PLUGIN_CONFIG_VIEW_H__

#include "nlohmann/json_fwd.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace yuan::plugin
{

class PluginConfigView
{
public:
    PluginConfigView() = default;
    explicit PluginConfigView(std::shared_ptr<const nlohmann::json> config);

    bool loaded() const;
    bool has(std::string_view path) const;

    std::string get_string(std::string_view path, std::string_view fallback = {}) const;
    bool get_bool(std::string_view path, bool fallback = false) const;
    std::int64_t get_int64(std::string_view path, std::int64_t fallback = 0) const;
    std::uint64_t get_uint64(std::string_view path, std::uint64_t fallback = 0) const;
    double get_double(std::string_view path, double fallback = 0.0) const;
    std::string dump(int indent = -1) const;

    const nlohmann::json *raw() const;

private:
    std::shared_ptr<const nlohmann::json> config_;
};

} // namespace yuan::plugin

#endif
