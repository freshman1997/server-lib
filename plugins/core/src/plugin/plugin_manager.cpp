#include "plugin/plugin_manager.h"
#include "plugin/plugin.h"
#include "plugin/plugin_symbol_solver.h"
#include "plugin/plugin_state.h"
#include "plugin/script_plugin_registry.h"
#include "plugin/extension_point_registry.h"
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
    typedef void *(*plugin_entry_function)(void);

    static constexpr int HOST_API_VERSION = 1;

    namespace
    {

#if defined(_WIN32)
        static constexpr const char *NATIVE_PLUGIN_EXT = ".dll";
#elif defined(__APPLE__)
        static constexpr const char *NATIVE_PLUGIN_EXT = ".dylib";
#else
        static constexpr const char *NATIVE_PLUGIN_EXT = ".so";
#endif

        static std::string join_path(const std::string &base, const std::string &relative)
        {
            return (std::filesystem::path(base) / relative).string();
        }

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
            if (!has_permission(context.granted_permissions, PluginPermission::use_network_runtime)) {
                context.network_runtime = nullptr;
            }
            if (!has_permission(context.granted_permissions, PluginPermission::use_extension_points)) {
                context.extension_point_registry = nullptr;
            }

            context.permission_guard = nullptr;
        }

    } // namespace

    class PluginManager::PluginData
    {
    public:
        PluginData() = default;
        ~PluginData() = default;

        void lock()
        {
            mutex_.lock();
        }
        void unlock()
        {
            mutex_.unlock();
        }

    public:
        mutable std::recursive_mutex mutex_;
        std::string plugin_path_;
        std::unordered_map<std::string, std::pair<void *, Plugin *> > plugins_;
        std::unordered_map<std::string, PluginContext> contexts_;
        PluginContext context_;
    };

    PluginManager::PluginManager()
        : data_(std::make_unique<PluginManager::PluginData>()), lifecycle_manager_()
    {
        lifecycle_manager_.set_state_change_callback(
            [](const std::string &name, PluginState old_state, PluginState new_state) {
                LOG_INFO("plugin '{}' lifecycle: {} -> {}",
                         name, to_string(old_state), to_string(new_state));
            });
    }

    PluginManager::~PluginManager()
    {
        release_all();
    }

    void PluginManager::set_plugin_path(const std::string & path)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        data_->plugin_path_ = std::filesystem::path(path).string();
    }
    void PluginManager::set_context(const PluginContext & context)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        data_->context_ = context;
        data_->context_.extension_point_registry = &extension_point_registry_;

        lifecycle_manager_.set_resource_guard(data_->context_.resource_guard);
        lifecycle_manager_.set_service_registry(data_->context_.service_registry);
        lifecycle_manager_.set_http_interceptor(data_->context_.http_interceptor);
        lifecycle_manager_.set_permission_guard(data_->context_.permission_guard);
        lifecycle_manager_.set_scheduler(data_->context_.scheduler);
        lifecycle_manager_.set_event_bus(data_->context_.event_bus);
    }

    const PluginContext &PluginManager::context() const
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        return data_->context_;
    }

    PluginContext PluginManager::plugin_context(const std::string & plugin_name) const
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        auto it = data_->contexts_.find(plugin_name);
        if (it != data_->contexts_.end()) {
            return it->second;
        }
        return make_plugin_context(plugin_name);
    }

    void *PluginManager::load_plugin_library(const std::string & plugin_name) const
    {
        const std::string real_name = join_path(data_->plugin_path_, plugin_name + NATIVE_PLUGIN_EXT);
        return PluginSymbolSolver::load_native_lib(real_name);
    }

    Plugin *PluginManager::create_plugin_instance(const std::string & plugin_name, void * handle) const
    {
        const std::string entry_symbol = "get_" + plugin_name + "_plugin_instance";

        plugin_entry_function func = nullptr;
        *(void **)(&func) = PluginSymbolSolver::find_symbol(handle, entry_symbol);
        if (!func) {
            return nullptr;
        }

        return static_cast<Plugin *>(func());
    }

    PluginConfigView PluginManager::load_plugin_config(const std::string & config_path) const
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

        try
        {
            auto config = std::make_shared<nlohmann::json>();
            config_stream >> *config;
            return PluginConfigView(std::move(config));
        }
        catch (const std::exception &ex)
        {
            LOG_ERROR("failed to parse plugin config '{}': {}", config_path, ex.what());
            return {};
        }
    }

    PluginContext PluginManager::make_plugin_context(const std::string & plugin_name) const
    {
        PluginContext context = data_->context_;
        context.plugin_name = plugin_name;
        context.plugin_root_path = data_->plugin_path_;
        context.extension_point_registry = &extension_point_registry_;
        context.storage = nullptr;

        auto config = find_plugin_manifest_config(plugin_name);
        if (!config.loaded()) {
            context.plugin_config_path = join_path(data_->plugin_path_, plugin_name + ".json");
        } else {
            context.config = std::move(config);
            std::string run_mode_str = context.config.get_string("run_mode", "");
            if (run_mode_str == "script") {
                context.plugin_root_path = join_path(data_->plugin_path_, plugin_name);
            }
        }

        if (context.config.loaded()) {
            auto perm_str = context.config.get_string("permissions", "");
            if (!perm_str.empty()) {
                context.granted_permissions = PluginPermissionNames::parse(perm_str);
            }
        }

        return context;
    }

    bool PluginManager::initialize_plugin(const std::string & plugin_name, Plugin * plugin)
    {
        plugin->on_loaded();

        auto context = make_plugin_context(plugin_name);

        if (context.granted_permissions == PluginPermission::none &&
            permission_guard_is_available(context)) {
            PluginMeta meta = plugin->meta();
            context.granted_permissions = meta.required_permissions;
        }

        if (context.permission_guard && context.granted_permissions != PluginPermission::none) {
            context.permission_guard->grant(plugin_name, context.granted_permissions);
            auto names = PluginPermissionNames::to_names(context.granted_permissions);
            std::string perm_str;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0)
                    perm_str += ",";
                perm_str += names[i];
            }
            LOG_INFO("plugin '{}' granted permissions: [{}]", plugin_name, perm_str);
        }

        apply_permission_boundary(context);

        PluginManifest manifest = plugin->manifest();
        if (manifest.api_version > HOST_API_VERSION) {
            LOG_ERROR("plugin '{}' requires api_version {} but host provides {}",
                      plugin_name, manifest.api_version, HOST_API_VERSION);
            return false;
        }

        for (const auto &ep : manifest.extension_points) {
            extension_point_registry_.register_extension_point(plugin_name, ep);
            LOG_INFO("plugin '{}' registered extension point '{}' (contract={}, v{})",
                     plugin_name, ep.name, ep.contract_id, ep.contract_version);
        }

        bool init_result = false;
        bool no_exception = lifecycle_manager_.call_guard().guarded_call_void(
            plugin_name, PluginState::loaded, "on_init",
            [&]() { init_result = plugin->on_init(context); });

        if (!no_exception || !init_result) {
            LOG_ERROR("plugin {} init failed!!!", plugin_name);
            return false;
        }

        data_->contexts_[plugin_name] = context;

        lifecycle_manager_.set_context(plugin_name, &data_->contexts_[plugin_name]);
        lifecycle_manager_.transition(plugin_name, PluginState::initialized);

        return true;
    }

    bool PluginManager::permission_guard_is_available(const PluginContext & ctx) const
    {
        return ctx.permission_guard != nullptr;
    }

    PluginConfigView PluginManager::find_plugin_manifest_config(const std::string & plugin_name) const
    {
        std::string json_path = join_path(data_->plugin_path_, plugin_name + ".json");
        auto config = load_plugin_config(json_path);
        if (config.loaded()) {
            return config;
        }

        std::string dir_json_path = join_path(join_path(data_->plugin_path_, plugin_name), "plugin.json");
        return load_plugin_config(dir_json_path);
    }

    bool PluginManager::load_native_plugin(const std::string & pluginName)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
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

        auto it = data_->plugins_.find(pluginName);
        if (it != data_->plugins_.end()) {
            release_plugin(pluginName);
        }
        data_->plugins_[pluginName] = { handle, plugin };

        lifecycle_manager_.register_instance(pluginName, plugin, handle);

        if (!initialize_plugin(pluginName, plugin)) {
            release_plugin(pluginName);
            return false;
        }

        return true;
    }

    bool PluginManager::load_script_plugin(const std::string & name, const PluginConfigView & config)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        PluginManifest manifest;
        manifest.plugin_id = name;
        manifest.name = config.get_string("name", name);
        manifest.version = config.get_string("version", "1.0.0");
        manifest.author = config.get_string("author", "");
        manifest.description = config.get_string("description", "");
        manifest.api_version = static_cast<int>(config.get_int64("api_version", 1));
        manifest.entry = config.get_string("entry", "main.lua");
        manifest.language = config.get_string("language", "lua");
        manifest.run_mode = PluginRunMode::script;

        std::string perm_str = config.get_string("permissions", "");
        if (!perm_str.empty()) {
            manifest.required_permissions = PluginPermissionNames::parse(perm_str);
        }

        auto *j = config.raw();
        if (j && j->contains("depends_on") && (*j)["depends_on"].is_array()) {
            for (const auto &item : (*j)["depends_on"]) {
                if (item.is_string()) {
                    manifest.depends_on.push_back(item.get<std::string>());
                }
            }
        }

        if (j && j->contains("extension_points") && (*j)["extension_points"].is_array()) {
            for (const auto &item : (*j)["extension_points"]) {
                if (!item.is_object())
                    continue;
                ExtensionPointDescriptor ep;
                ep.name = item.value("name", "");
                ep.type = item.value("type", "");
                ep.contract_id = item.value("contract_id", "");
                ep.contract_version = item.value("contract_version", 1);
                if (!ep.name.empty()) {
                    manifest.extension_points.push_back(std::move(ep));
                }
            }
        }

        if (manifest.api_version > HOST_API_VERSION) {
            LOG_ERROR("script plugin '{}' requires api_version {} but host provides {}",
                      name, manifest.api_version, HOST_API_VERSION);
            return false;
        }

        std::string language = manifest.language.empty() ? "lua" : manifest.language;
        auto *adapter = ScriptPluginRegistry::instance().create(language, manifest, config);
        if (!adapter) {
            LOG_ERROR("no script adapter registered for language '{}', cannot load plugin '{}'",
                      language, name);
            return false;
        }

        std::string script_path = join_path(join_path(data_->plugin_path_, name), manifest.entry);
        if (!adapter->load_script(script_path)) {
            delete adapter;
            return false;
        }

        LOG_INFO("load script plugin {} success!", name);

        auto it = data_->plugins_.find(name);
        if (it != data_->plugins_.end()) {
            release_plugin(name);
        }
        data_->plugins_[name] = { nullptr, adapter };

        lifecycle_manager_.register_instance(name, adapter, nullptr);

        if (!initialize_plugin(name, adapter)) {
            release_plugin(name);
            return false;
        }

        return true;
    }

    bool PluginManager::load(const std::string & pluginName)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        auto config = find_plugin_manifest_config(pluginName);

        if (config.loaded()) {
            std::string run_mode_str = config.get_string("run_mode", "");
            if (run_mode_str == "script") {
                return load_script_plugin(pluginName, config);
            }
        }

        return load_native_plugin(pluginName);
    }

    bool PluginManager::load_all(const std::vector<std::string> & plugin_names)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        if (plugin_names.empty()) {
            return true;
        }

        struct PendingPlugin
        {
            std::string name;
            Plugin *plugin = nullptr;
            void *handle = nullptr;
            bool is_script = false;
            PluginConfigView config;
            std::vector<std::string> depends_on;
        };

        std::vector<PendingPlugin> pending;
        pending.reserve(plugin_names.size());

        for (const auto &name : plugin_names) {
            if (data_->plugins_.count(name)) {
                LOG_WARN("plugin '{}' already loaded, skipping", name);
                continue;
            }

            auto config = find_plugin_manifest_config(name);
            std::string run_mode_str = config.get_string("run_mode", "");

            if (config.loaded() && run_mode_str == "script") {
                std::vector<std::string> deps;
                auto *j = config.raw();
                if (j && j->contains("depends_on") && (*j)["depends_on"].is_array()) {
                    for (const auto &item : (*j)["depends_on"]) {
                        if (item.is_string()) {
                            deps.push_back(item.get<std::string>());
                        }
                    }
                }
                pending.push_back({ name, nullptr, nullptr, true, config, deps });
            } else {
                void *handle = load_plugin_library(name);
                if (!handle) {
                    LOG_ERROR("failed to load plugin library '{}'", name);
                    for (auto &p : pending) {
                        if (p.handle) {
                            PluginSymbolSolver::release_native_lib(p.handle);
                        }
                    }
                    return false;
                }

                Plugin *plugin = create_plugin_instance(name, handle);
                if (!plugin) {
                    LOG_ERROR("failed to create plugin instance '{}'", name);
                    PluginSymbolSolver::release_native_lib(handle);
                    for (auto &p : pending) {
                        if (p.handle) {
                            PluginSymbolSolver::release_native_lib(p.handle);
                        }
                    }
                    return false;
                }

                PluginManifest manifest = plugin->manifest();
                if (manifest.api_version > HOST_API_VERSION) {
                    LOG_ERROR("plugin '{}' requires api_version {} but host provides {}, skipping",
                              name, manifest.api_version, HOST_API_VERSION);
                    delete plugin;
                    PluginSymbolSolver::release_native_lib(handle);
                    continue;
                }

                PluginMeta meta = plugin->meta();
                meta.name = name;
                pending.push_back({ name, plugin, handle, false, {}, meta.depends_on });
            }
        }

        std::vector<std::string> sorted_names;
        sorted_names.reserve(pending.size());
        for (const auto &p : pending) {
            sorted_names.push_back(p.name);
        }

        std::vector<std::string> sorted;
        std::unordered_map<std::string, std::vector<std::string> > pending_deps;
        for (const auto &p : pending) {
            pending_deps[p.name] = p.depends_on;
        }
        if (!topological_sort(sorted_names, sorted, pending_deps)) {
            LOG_ERROR("circular dependency detected among plugins");
            for (auto &p : pending) {
                if (p.plugin) {
                    p.plugin->on_release();
                    delete p.plugin;
                }
                if (p.handle) {
                    PluginSymbolSolver::release_native_lib(p.handle);
                }
            }
            return false;
        }

        std::unordered_map<std::string, PendingPlugin *> pending_index;
        for (auto &p : pending) {
            pending_index[p.name] = &p;
        }

        std::unordered_set<std::string> loaded_during_this_call;

        auto rollback = [&]() {
            for (const auto &loaded_name : loaded_during_this_call) {
                release_plugin(loaded_name);
            }
            for (auto &up : pending) {
                if (!loaded_during_this_call.count(up.name)) {
                    if (up.plugin) {
                        up.plugin->on_release();
                        delete up.plugin;
                    }
                    if (up.handle) {
                        PluginSymbolSolver::release_native_lib(up.handle);
                    }
                }
            }
        };

        for (const auto &name : sorted) {
            auto it = pending_index.find(name);
            if (it == pending_index.end()) {
                continue;
            }

            auto &p = *it->second;

            if (p.is_script) {
                if (!load_script_plugin(p.name, p.config)) {
                    LOG_ERROR("plugin '{}' (script) load failed, rolling back", p.name);
                    rollback();
                    return false;
                }
                loaded_during_this_call.insert(p.name);
            } else {
                data_->plugins_[p.name] = { p.handle, p.plugin };
                loaded_during_this_call.insert(p.name);

                lifecycle_manager_.register_instance(p.name, p.plugin, p.handle);

                if (!initialize_plugin(p.name, p.plugin)) {
                    LOG_ERROR("plugin '{}' init failed, rolling back", p.name);
                    rollback();
                    return false;
                }
            }

            LOG_INFO("load {} success!", name);
        }

        return true;
    }

    Plugin *PluginManager::get_plugin(const std::string & name)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        auto it = data_->plugins_.find(name);
        return it == data_->plugins_.end() ? nullptr : it->second.second;
    }

    void PluginManager::release_plugin(const std::string & pluginName)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        auto it = data_->plugins_.find(pluginName);
        if (it == data_->plugins_.end()) {
            return;
        }

        extension_point_registry_.unregister_extension_points(pluginName);

        auto plugin_state = lifecycle_manager_.state(pluginName);
        if (is_operational(plugin_state)) {
            auto *plugin = it->second.second;
            if (plugin) {
                try
                {
                    plugin->on_disable();
                }
                catch (const std::exception &ex)
                {
                    LOG_ERROR("plugin '{}' on_disable() threw: {}", pluginName, ex.what());
                }
                catch (...)
                {
                    LOG_ERROR("plugin '{}' on_disable() threw unknown exception", pluginName);
                }
            }
        }

        lifecycle_manager_.stop(pluginName);
        lifecycle_manager_.unload(pluginName);

        data_->plugins_.erase(it);
        data_->contexts_.erase(pluginName);
    }

    void PluginManager::release_all()
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        std::vector<std::string> pluginNames;
        pluginNames.reserve(data_->plugins_.size());
        for (const auto &item : data_->plugins_) {
            pluginNames.push_back(item.first);
        }

        for (const auto &pluginName : pluginNames) {
            release_plugin(pluginName);
        }
    }

    PluginConfigView PluginManager::reload_plugin_config(const std::string & plugin_name) const
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        auto it = data_->plugins_.find(plugin_name);
        if (it == data_->plugins_.end()) {
            return {};
        }

        const std::string config_path = join_path(data_->plugin_path_, plugin_name + ".json");
        return load_plugin_config(config_path);
    }

    void PluginManager::set_plugin_storage(const std::string & plugin_name, HostStorage * storage)
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        auto it = data_->contexts_.find(plugin_name);
        if (it != data_->contexts_.end()) {
            it->second.storage = storage;
        }
    }

    std::vector<std::string> PluginManager::loaded_plugin_names() const
    {
        std::lock_guard<std::recursive_mutex> lock(data_->mutex_);
        std::vector<std::string> names;
        names.reserve(data_->plugins_.size());
        for (const auto & [
                              name,
                              _
                          ] : data_->plugins_) {
            names.push_back(name);
        }
        return names;
    }

    PluginLifecycleManager &PluginManager::lifecycle_manager()
    {
        return lifecycle_manager_;
    }

    const PluginLifecycleManager &PluginManager::lifecycle_manager() const
    {
        return lifecycle_manager_;
    }

    ExtensionPointRegistry &PluginManager::extension_point_registry()
    {
        return extension_point_registry_;
    }

    const ExtensionPointRegistry &PluginManager::extension_point_registry() const
    {
        return extension_point_registry_;
    }

    bool PluginManager::topological_sort(const std::vector<std::string> & names,
                                         std::vector<std::string> & sorted,
                                         const std::unordered_map<std::string, std::vector<std::string> > & external_deps) const
    {
        if (names.empty()) {
            return true;
        }

        std::unordered_map<std::string, std::vector<std::string> > deps;
        std::unordered_set<std::string> all_names(names.begin(), names.end());

        for (const auto &name : names) {
            deps[name] = {};
            auto ext_it = external_deps.find(name);
            if (ext_it != external_deps.end()) {
                for (const auto &dep : ext_it->second) {
                    if (all_names.count(dep)) {
                        deps[name].push_back(dep);
                    }
                }
            } else {
                auto it = data_->plugins_.find(name);
                if (it != data_->plugins_.end()) {
                    PluginMeta meta = it->second.second->meta();
                    for (const auto &dep : meta.depends_on) {
                        if (all_names.count(dep)) {
                            deps[name].push_back(dep);
                        }
                    }
                }
            }
        }

        std::unordered_map<std::string, int> in_degree;
        for (const auto &name : names) {
            in_degree[name] = 0;
        }
        for (const auto & [
                              name,
                              dep_list
                          ] : deps) {
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

        return sorted.size() == names.size();
    }
}
