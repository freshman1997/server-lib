#include "plugin/plugin_manager.h"
#include "plugin/plugin.h"
#include "plugin/plugin_symbol_solver.h"
#include "logger.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace yuan::plugin 
{
    typedef void * (*plugin_entry_function)(void);

    namespace
    {

    void apply_permission_boundary(PluginContext &context)
    {
        if (!has_permission(context.granted_permissions, PluginPermission::use_event_bus)) {
            context.event_bus = nullptr;
        }
        if (!has_permission(context.granted_permissions, PluginPermission::use_logger)) {
            context.logger = nullptr;
        }
        if (!has_permission(context.granted_permissions, PluginPermission::use_service_catalog)) {
            context.service_catalog = nullptr;
        }
        if (!has_permission(context.granted_permissions, PluginPermission::use_scheduler)) {
            context.scheduler = nullptr;
        }
        if (!has_permission(context.granted_permissions, PluginPermission::use_service_registry)) {
            context.service_registry = nullptr;
        }
        if (!has_permission(context.granted_permissions, PluginPermission::use_http_intercept)) {
            context.http_interceptor = nullptr;
        }
        if (!has_permission(context.granted_permissions, PluginPermission::use_storage)) {
            context.storage = nullptr;
        }

        // Plugins should not be able to escalate their own permissions through the raw host guard.
        context.permission_guard = nullptr;
    }

    } // namespace

    class PluginManager::PluginData
    {
    public:
        PluginData() = default;
        ~PluginData()
        {
        }

    public:
        std::string plugin_path_;
        std::unordered_map<std::string, std::pair<void *, Plugin *>> plugins_;
        std::unordered_map<std::string, PluginContext> contexts_;
        PluginContext context_;
    };

    PluginManager::PluginManager() : data_(std::make_unique<PluginManager::PluginData>()) {}

    PluginManager::~PluginManager()
    {
        release_all();
    }

    void PluginManager::set_plugin_path(const std::string &path)
    {
        data_->plugin_path_ = path;
        if (!path.empty() && !path.ends_with('/') && !path.ends_with('\\'))
        {
            data_->plugin_path_.push_back('/');
        }
    }
    void PluginManager::set_context(PluginContext context)
    {
        data_->context_ = std::move(context);
    }

    const PluginContext& PluginManager::context() const
    {
        return data_->context_;
    }

    PluginContext PluginManager::plugin_context(const std::string &plugin_name) const
    {
        auto it = data_->contexts_.find(plugin_name);
        if (it != data_->contexts_.end()) {
            return it->second;
        }
        return make_plugin_context(plugin_name);
    }

    void *PluginManager::load_plugin_library(const std::string &plugin_name) const
    {
        const std::string real_name = data_->plugin_path_ + plugin_name + ".plugin";
        return PluginSymbolSolver::load_native_lib(real_name);
    }

    Plugin *PluginManager::create_plugin_instance(const std::string &plugin_name, void *handle) const
    {
        const std::string entry_symbol = "get_" + plugin_name + "_plugin_instance";

        plugin_entry_function func = nullptr;
        *(void **)(&func) = PluginSymbolSolver::find_symbol(handle, entry_symbol);
        if (!func) {
            return nullptr;
        }

        return static_cast<Plugin *>(func());
    }

    PluginConfigView PluginManager::load_plugin_config(const std::string &config_path) const
    {
        if (config_path.empty()) {
            return {};
        }

        std::error_code ec;
        if (!std::filesystem::exists(config_path, ec) || ec) {
            return {};
        }

        std::ifstream config_stream(config_path);
        if (!config_stream.good()) {
            LOG_WARN("failed to open plugin config '{}'", config_path);
            return {};
        }

        try {
            auto config = std::make_shared<nlohmann::json>();
            config_stream >> *config;
            return PluginConfigView(std::move(config));
        } catch (const std::exception &ex) {
            LOG_ERROR("failed to parse plugin config '{}': {}", config_path, ex.what());
            return {};
        }
    }

    PluginContext PluginManager::make_plugin_context(const std::string &plugin_name) const
    {
        PluginContext context = data_->context_;
        context.plugin_name = plugin_name;
        context.plugin_root_path = data_->plugin_path_;
        context.plugin_config_path = data_->plugin_path_ + plugin_name + ".json";
        context.config = load_plugin_config(context.plugin_config_path);

        // 从配置文件中读取 permissions 字段覆盖 meta 中的声明
        // 配置文件中的 permissions 优先级最高 (宿主控制)
        if (context.config.loaded()) {
            auto perm_str = context.config.get_string("permissions", "");
            if (!perm_str.empty()) {
                context.granted_permissions = PluginPermissionNames::parse(perm_str);
            }
        }

        return context;
    }

    bool PluginManager::initialize_plugin(const std::string &plugin_name, Plugin *plugin)
    {
        plugin->on_loaded();

        auto context = make_plugin_context(plugin_name);

        // 授予权限: 如果配置文件未指定 permissions, 则使用 meta 中声明的 required_permissions
        if (context.granted_permissions == PluginPermission::none &&
            permission_guard_is_available(context))
        {
            PluginMeta meta = plugin->meta();
            context.granted_permissions = meta.required_permissions;
        }

        // 通过权限守卫记录授予的权限
        if (context.permission_guard && context.granted_permissions != PluginPermission::none) {
            context.permission_guard->grant(plugin_name, context.granted_permissions);
            auto names = PluginPermissionNames::to_names(context.granted_permissions);
            std::string perm_str;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) perm_str += ",";
                perm_str += names[i];
            }
            LOG_INFO("plugin '{}' granted permissions: [{}]", plugin_name, perm_str);
        }

        apply_permission_boundary(context);

        if (!plugin->on_init(context)) {
            LOG_ERROR("plugin {} init failed!!!", plugin_name);
            return false;
        }

        data_->contexts_[plugin_name] = context;
        return true;
    }

    bool PluginManager::permission_guard_is_available(const PluginContext &ctx) const
    {
        return ctx.permission_guard != nullptr;
    }


    bool PluginManager::load(const std::string &pluginName)
    {
        void *handle = load_plugin_library(pluginName);
        if (!handle) {
            return false;
        }

        Plugin *plugin = create_plugin_instance(pluginName, handle);
        if (!plugin) {
            PluginSymbolSolver::release_native_lib(handle);
            return false;
        }

        LOG_INFO("load {} success!", pluginName);

        add_plugin(pluginName, plugin, handle);

        if (!initialize_plugin(pluginName, plugin)) {
            release_plugin(pluginName);
            return false;
        }

        return true;
    }

    bool PluginManager::load_all(const std::vector<std::string> &plugin_names)
    {
        if (plugin_names.empty()) {
            return true;
        }

        // 第一阶段: 加载所有插件库但不初始化, 获取 meta 信息用于依赖排序
        struct PendingPlugin
        {
            std::string name;
            Plugin *plugin;
            void *handle;
            PluginMeta meta;
        };

        std::vector<PendingPlugin> pending;
        pending.reserve(plugin_names.size());

        for (const auto &name : plugin_names) {
            // 检查是否已加载
            if (data_->plugins_.count(name)) {
                LOG_WARN("plugin '{}' already loaded, skipping", name);
                continue;
            }

            void *handle = load_plugin_library(name);
            if (!handle) {
                LOG_ERROR("failed to load plugin library '{}'", name);
                // 回滚已加载但未初始化的
                for (auto &p : pending) {
                    PluginSymbolSolver::release_native_lib(p.handle);
                }
                return false;
            }

            Plugin *plugin = create_plugin_instance(name, handle);
            if (!plugin) {
                LOG_ERROR("failed to create plugin instance '{}'", name);
                PluginSymbolSolver::release_native_lib(handle);
                for (auto &p : pending) {
                    PluginSymbolSolver::release_native_lib(p.handle);
                }
                return false;
            }

            PluginMeta meta = plugin->meta();
            meta.name = name; // 确保名称一致
            pending.push_back({name, plugin, handle, std::move(meta)});
        }

        // 第二阶段: 拓扑排序
        std::vector<std::string> sorted_names;
        sorted_names.reserve(pending.size());
        for (const auto &p : pending) {
            sorted_names.push_back(p.name);
        }

        std::vector<std::string> sorted;
        if (!topological_sort(sorted_names, sorted)) {
            LOG_ERROR("circular dependency detected among plugins");
            for (auto &p : pending) {
                p.plugin->on_release();
                delete p.plugin;
                PluginSymbolSolver::release_native_lib(p.handle);
            }
            return false;
        }

        // 建立 name -> PendingPlugin 索引
        std::unordered_map<std::string, PendingPlugin *> pending_index;
        for (auto &p : pending) {
            pending_index[p.name] = &p;
        }

        // 第三阶段: 按拓扑顺序初始化
        for (const auto &name : sorted) {
            auto it = pending_index.find(name);
            if (it == pending_index.end()) {
                continue;
            }

            auto &p = *it->second;
            add_plugin(p.name, p.plugin, p.handle);

            if (!initialize_plugin(p.name, p.plugin)) {
                LOG_ERROR("plugin '{}' init failed, rolling back", p.name);
                // 逆序释放已初始化的
                for (auto rit = sorted.rbegin(); rit != sorted.rend(); ++rit) {
                    if (data_->plugins_.count(*rit)) {
                        release_plugin(*rit);
                    }
                }
                // 释放未初始化的
                for (auto &up : pending) {
                    if (!data_->plugins_.count(up.name)) {
                        // 尚未 add_plugin 的, 直接释放库
                        PluginSymbolSolver::release_native_lib(up.handle);
                    }
                }
                return false;
            }

            LOG_INFO("load {} success!", name);
        }

        return true;
    }

    void PluginManager::add_plugin(const std::string &name, Plugin *plugin, void *handle)
    {
        auto it = data_->plugins_.find(name);
        if (it != data_->plugins_.end()) {
            release_plugin(name);
        }
        data_->plugins_[name] = {handle, plugin};
    }

    Plugin * PluginManager::get_plugin(const std::string &name)
    {
        auto it = data_->plugins_.find(name);
        return it == data_->plugins_.end() ? nullptr : it->second.second;
    }

    void PluginManager::release_plugin(const std::string &pluginName)
    {
        auto it = data_->plugins_.find(pluginName);
        if (it == data_->plugins_.end()) {
            return;
        }

        auto plugin = it->second.second;
        if (plugin)
        {
            plugin->on_release();
            delete plugin;
        }

        PluginSymbolSolver::release_native_lib(it->second.first);

        data_->plugins_.erase(it);
        data_->contexts_.erase(pluginName);
    }

    void PluginManager::release_all()
    {
        std::vector<std::string> pluginNames;
        pluginNames.reserve(data_->plugins_.size());
        for (const auto &item : data_->plugins_) {
            pluginNames.push_back(item.first);
        }

        for (const auto &pluginName : pluginNames) {
            release_plugin(pluginName);
        }
    }

    PluginConfigView PluginManager::reload_plugin_config(const std::string &plugin_name)
    {
        auto it = data_->plugins_.find(plugin_name);
        if (it == data_->plugins_.end()) {
            return {};
        }

        const std::string config_path = data_->plugin_path_ + plugin_name + ".json";
        return load_plugin_config(config_path);
    }

    std::vector<std::string> PluginManager::loaded_plugin_names() const
    {
        std::vector<std::string> names;
        names.reserve(data_->plugins_.size());
        for (const auto &[name, _] : data_->plugins_) {
            names.push_back(name);
        }
        return names;
    }

    bool PluginManager::topological_sort(const std::vector<std::string> &names,
                                          std::vector<std::string> &sorted) const
    {
        if (names.empty()) {
            return true;
        }

        // 收集所有插件的 meta 信息
        std::unordered_map<std::string, std::vector<std::string>> deps;
        std::unordered_set<std::string> all_names(names.begin(), names.end());

        for (const auto &name : names) {
            auto it = data_->plugins_.find(name);
            if (it == data_->plugins_.end()) {
                continue;
            }
            PluginMeta meta = it->second.second->meta();
            deps[name] = {}; // 确保每个插件都有条目
            for (const auto &dep : meta.depends_on) {
                if (all_names.count(dep)) {
                    deps[name].push_back(dep);
                }
                // 不在待加载列表中的依赖视为外部依赖 (已加载或由宿主提供), 忽略
            }
        }

        // Kahn 算法
        std::unordered_map<std::string, int> in_degree;
        for (const auto &name : names) {
            in_degree[name] = 0;
        }
        for (const auto &[name, dep_list] : deps) {
            in_degree[name] = static_cast<int>(dep_list.size());
        }

        std::queue<std::string> queue;
        for (const auto &name : names) {
            if (in_degree[name] == 0) {
                queue.push(name);
            }
        }

        sorted.clear();
        sorted.reserve(names.size());
        while (!queue.empty()) {
            std::string current = std::move(queue.front());
            queue.pop();
            sorted.push_back(current);

            for (const auto &name : names) {
                auto &dep_list = deps[name];
                for (const auto &dep : dep_list) {
                    if (dep == current) {
                        --in_degree[name];
                        if (in_degree[name] == 0) {
                            queue.push(name);
                        }
                    }
                }
            }
        }

        return sorted.size() == names.size(); // 不等则说明有循环依赖
    }
}
