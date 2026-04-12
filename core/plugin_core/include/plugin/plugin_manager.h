#ifndef __PLUGIN_MANAGER_H__
#define __PLUGIN_MANAGER_H__
#include "plugin/plugin.h"
#include "plugin/plugin_context.h"
#include "singleton/singleton.h"

#include <memory>
#include <string>
#include <vector>

namespace yuan::plugin 
{
    class PluginManager  : public singleton::Singleton<PluginManager>
    {
    public:
        PluginManager();
        ~PluginManager();

    public:
        void set_plugin_path(const std::string &path);

        void set_context(PluginContext context);

        const PluginContext& context() const;
        PluginContext plugin_context(const std::string &plugin_name) const;

        /// 加载单个插件
        bool load(const std::string &pluginName);

        /// 按依赖拓扑排序批量加载 (根据 PluginMeta::depends_on)
        bool load_all(const std::vector<std::string> &plugin_names);

        Plugin * get_plugin(const std::string &name);

        void release_plugin(const std::string &pluginName);

        void release_all();

        /// 重新加载指定插件的配置文件并返回新的 PluginConfigView
        PluginConfigView reload_plugin_config(const std::string &plugin_name);

        /// 获取所有已加载插件的名称
        std::vector<std::string> loaded_plugin_names() const;

    private:
        void add_plugin(const std::string &name, Plugin *plugin, void *handle);
        void *load_plugin_library(const std::string &plugin_name) const;
        Plugin *create_plugin_instance(const std::string &plugin_name, void *handle) const;
        PluginConfigView load_plugin_config(const std::string &config_path) const;
        PluginContext make_plugin_context(const std::string &plugin_name) const;
        bool initialize_plugin(const std::string &plugin_name, Plugin *plugin);

        /// 拓扑排序, 返回排序后的名称列表; 检测到循环依赖时返回 false
        bool topological_sort(const std::vector<std::string> &names,
                              std::vector<std::string> &sorted) const;

        bool permission_guard_is_available(const PluginContext &ctx) const;

    private:
        class PluginData;
        std::unique_ptr<PluginData> data_;
    };
}
#endif // __PLUGIN_MANAGER_H__
