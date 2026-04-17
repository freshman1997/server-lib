#ifndef __HELLOWORLD_H__
#define __HELLOWORLD_H__
#include "plugin/plugin.h"
#include "plugin/host_event_bus.h"
#include "plugin/host_scheduler.h"
#include "plugin/host_service_registry.h"
#include "plugin/host_resource_guard.h"
#include "plugin/host_http_interceptor.h"
#include "plugin/host_storage.h"

class HelloWorldPlugin : public yuan::plugin::Plugin
{
public:
    HelloWorldPlugin();
    ~HelloWorldPlugin() override;

public:
    void on_loaded() override;
    bool on_init(const yuan::plugin::PluginContext &context) override;
    void on_release() override;
    bool on_health_check() const override;
    void on_config_changed(const yuan::plugin::PluginConfigView &config) override;
    yuan::plugin::PluginMeta meta() const override;

private:
    yuan::plugin::HostEventBus *event_bus_ = nullptr;
    yuan::plugin::HostLogger *logger_ = nullptr;
    yuan::plugin::HostScheduler *scheduler_ = nullptr;
    yuan::plugin::HostServiceRegistry *service_registry_ = nullptr;
    yuan::plugin::HostResourceGuard *resource_guard_ = nullptr;
    yuan::plugin::HostHttpInterceptor *http_interceptor_ = nullptr;
    yuan::plugin::HostStorage *storage_ = nullptr;

    // 使用 context 便捷方法注册的资源不再需要手动保存 token
    // resource_guard 会在卸载时自动清理

    // 配置缓存
    bool enabled_ = true;
    std::string version_ = "n/a";
};

#endif // __HELLOWORLD_H__
