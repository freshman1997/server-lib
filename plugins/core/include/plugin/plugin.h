#ifndef __PLUGIN_H__
#define __PLUGIN_H__
#include "plugin/plugin_context.h"
#include "plugin/plugin_manifest.h"
#include "plugin/plugin_meta.h"

namespace yuan::plugin
{
    class Plugin
    {
    public:
        virtual ~Plugin() = default;

        /// 库加载后立即调用, 仅做最轻量的初始化
        virtual void on_loaded() = 0;

        /// 接收完整上下文并初始化, 返回 false 表示初始化失败
        virtual bool on_init(const PluginContext &context) = 0;

        /// 插件已激活, 可正常提供服务
        virtual void on_enable()
        {
        }

        /// 插件即将停用, 停止接受新入口
        virtual void on_disable()
        {
        }

        /// 卸载前调用, 释放所有资源
        virtual void on_release() = 0;

        /// 健康检查, 返回 true 表示插件状态正常 (默认返回 true)
        virtual bool on_health_check() const
        {
            return true;
        }

        /// 配置变更通知, 插件可选择性地热更新内部状态
        virtual void on_config_changed(const PluginConfigView & /*config*/)
        {
        }

        /// 返回插件元数据 (名称/版本/作者/依赖等)
        virtual PluginMeta meta() const
        {
            return {};
        }

        /// 返回插件清单 (完整声明信息)
        virtual PluginManifest manifest() const
        {
            return meta().to_manifest();
        }
    };
}

#endif // __PLUGIN_H__
