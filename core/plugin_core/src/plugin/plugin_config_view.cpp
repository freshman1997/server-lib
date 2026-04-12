#include "plugin/plugin_config_view.h"

#include "nlohmann/json.hpp"

#include <string>

namespace yuan::plugin
{

namespace
{

const nlohmann::json *resolve_path(const std::shared_ptr<const nlohmann::json> &config, std::string_view path)
{
    if (!config) {
        return nullptr;
    }

    const nlohmann::json *current = config.get();
    if (path.empty()) {
        return current;
    }

    std::size_t start = 0;
    while (start < path.size()) {
        const auto dot = path.find('.', start);
        const auto len = dot == std::string_view::npos ? path.size() - start : dot - start;
        if (len == 0 || !current->is_object()) {
            return nullptr;
        }

        const std::string key(path.substr(start, len));
        const auto it = current->find(key);
        if (it == current->end()) {
            return nullptr;
        }

        current = &(*it);
        if (dot == std::string_view::npos) {
            return current;
        }
        start = dot + 1;
    }

    return current;
}

} // namespace

PluginConfigView::PluginConfigView(std::shared_ptr<const nlohmann::json> config)
    : config_(std::move(config))
{
}

bool PluginConfigView::loaded() const
{
    return static_cast<bool>(config_);
}

bool PluginConfigView::has(std::string_view path) const
{
    return resolve_path(config_, path) != nullptr;
}

std::string PluginConfigView::get_string(std::string_view path, std::string_view fallback) const
{
    if (const auto *value = resolve_path(config_, path); value && value->is_string()) {
        return value->get<std::string>();
    }
    return std::string(fallback);
}

bool PluginConfigView::get_bool(std::string_view path, bool fallback) const
{
    if (const auto *value = resolve_path(config_, path); value && value->is_boolean()) {
        return value->get<bool>();
    }
    return fallback;
}

std::int64_t PluginConfigView::get_int64(std::string_view path, std::int64_t fallback) const
{
    if (const auto *value = resolve_path(config_, path); value && value->is_number_integer()) {
        return value->get<std::int64_t>();
    }
    return fallback;
}

std::uint64_t PluginConfigView::get_uint64(std::string_view path, std::uint64_t fallback) const
{
    if (const auto *value = resolve_path(config_, path); value && value->is_number_unsigned()) {
        return value->get<std::uint64_t>();
    }
    return fallback;
}

double PluginConfigView::get_double(std::string_view path, double fallback) const
{
    if (const auto *value = resolve_path(config_, path); value && value->is_number()) {
        return value->get<double>();
    }
    return fallback;
}

std::string PluginConfigView::dump(int indent) const
{
    if (!config_) {
        return {};
    }
    return config_->dump(indent);
}

const nlohmann::json *PluginConfigView::raw() const
{
    return config_.get();
}

} // namespace yuan::plugin
