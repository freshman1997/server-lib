#include "helloworld.h"
#include "api/api.h"
#include "app_events.h"
#include "plugin/plugin_events.h"
#include "plugin/plugin_permission.h"

#include <any>
#include <atomic>
#include <iostream>
#include <memory>

namespace
{

inline constexpr const char *kGreetingServiceContract = "plugin.helloworld.greeting";

class GreetingService : public yuan::plugin::PluginService
{
public:
    std::function<std::string(const std::string &)> greet = [](const std::string &name) {
        return "Hello, " + name + "! from HelloWorld plugin";
    };

    bool init(const yuan::plugin::PluginContext &context) override
    {
        plugin_name_ = context.plugin_name;
        scheduler_ = context.scheduler;
        resource_guard_ = context.resource_guard;
        logger_ = context.logger;
        return true;
    }

    void start() override
    {
        running_->store(true);

        if (!scheduler_) {
            return;
        }

        auto running = running_;
        task_id_ = scheduler_->schedule_interval(
            std::chrono::seconds(5),
            [running, logger = logger_]() {
                if (!running->load()) {
                    return;
                }
                if (logger) {
                    logger->log(
                        yuan::plugin::HostLogLevel::debug,
                        __FILE__,
                        __LINE__,
                        __func__,
                        "managed greeting service tick");
                }
                std::cout << "[HelloWorld] managed greeting service tick\n";
            },
            "HelloWorld.greeting.service");

        if (resource_guard_ && task_id_ != 0) {
            auto *scheduler = scheduler_;
            auto task_id = task_id_;
            resource_id_ = resource_guard_->track(
                plugin_name_,
                yuan::plugin::PluginResourceType::scheduler_task,
                [scheduler, task_id]() {
                    if (scheduler) {
                        scheduler->cancel(task_id);
                    }
                },
                "task:HelloWorld.greeting.service");
        }
    }

    void stop() override
    {
        running_->store(false);

        if (scheduler_ && task_id_ != 0) {
            scheduler_->cancel(task_id_);
            task_id_ = 0;
        }

        if (resource_guard_ && resource_id_ != 0) {
            resource_guard_->untrack(resource_id_);
            resource_id_ = 0;
        }

        std::cout << "[HelloWorld] managed greeting service stopped\n";
    }

private:
    std::string plugin_name_;
    yuan::plugin::HostScheduler *scheduler_ = nullptr;
    yuan::plugin::HostResourceGuard *resource_guard_ = nullptr;
    yuan::plugin::HostLogger *logger_ = nullptr;
    yuan::plugin::HostSchedulerTaskId task_id_ = 0;
    uint64_t resource_id_ = 0;
    std::shared_ptr<std::atomic_bool> running_ = std::make_shared<std::atomic_bool>(false);
};

} // namespace

HelloWorldPlugin::HelloWorldPlugin()
{
}

HelloWorldPlugin::~HelloWorldPlugin()
{
}

void HelloWorldPlugin::on_loaded()
{
    std::cout << "hello world on_loaded !!\n";
}

yuan::plugin::PluginMeta HelloWorldPlugin::meta() const
{
    yuan::plugin::PluginMeta m;
    m.name = "HelloWorld";
    m.version = "3.0.0";
    m.author = "yuan";
    m.description = "Demo plugin showcasing all plugin system features";
    m.api_version = 1;
    m.required_permissions = yuan::plugin::PluginPermission::use_event_bus
                          | yuan::plugin::PluginPermission::use_logger
                          | yuan::plugin::PluginPermission::use_scheduler
                          | yuan::plugin::PluginPermission::use_service_catalog
                          | yuan::plugin::PluginPermission::use_service_registry
                          | yuan::plugin::PluginPermission::use_http_intercept
                          | yuan::plugin::PluginPermission::use_storage;
    return m;
}

bool HelloWorldPlugin::on_init(const yuan::plugin::PluginContext &context)
{
    yuan::plugin::PluginContextHelper helper(context);
    event_bus_ = context.event_bus;
    logger_ = context.logger;
    scheduler_ = context.scheduler;
    service_registry_ = context.service_registry;
    resource_guard_ = context.resource_guard;
    http_interceptor_ = context.http_interceptor;
    storage_ = context.storage;

    if (!event_bus_) {
        std::cerr << "event bus is null!!! \n";
        return false;
    }

    std::cout << "hello world init success !! app=" << context.app_name
              << " plugin=" << context.plugin_name
              << " resource_guard=" << (resource_guard_ ? "true" : "false")
              << " http_interceptor=" << (http_interceptor_ ? "true" : "false")
              << " storage=" << (storage_ ? "true" : "false")
              << "\n";

    const auto capabilities = context.capabilities();
    std::cout << "[HelloWorld] capability snapshot: "
              << "event_bus=" << (capabilities.event_bus ? "true" : "false")
              << " logger=" << (capabilities.logger ? "true" : "false")
              << " scheduler=" << (capabilities.scheduler ? "true" : "false")
              << " service_catalog=" << (capabilities.service_catalog ? "true" : "false")
              << " service_registry=" << (capabilities.service_registry ? "true" : "false")
              << " http_interceptor=" << (capabilities.http_interceptor ? "true" : "false")
              << " storage=" << (capabilities.storage ? "true" : "false")
              << "\n";

    // ---- 权限检查演示 ----
    auto can_use = [&](yuan::plugin::PluginPermission perm, const char *name) {
        bool ok = yuan::plugin::has_permission(context.granted_permissions, perm);
        std::cout << "[HelloWorld] permission check: " << name << "=" << (ok ? "granted" : "denied") << "\n";
        return ok;
    };
    can_use(yuan::plugin::PluginPermission::use_event_bus, "use_event_bus");
    can_use(yuan::plugin::PluginPermission::use_logger, "use_logger");
    can_use(yuan::plugin::PluginPermission::use_scheduler, "use_scheduler");
    can_use(yuan::plugin::PluginPermission::use_service_catalog, "use_service_catalog");
    can_use(yuan::plugin::PluginPermission::use_service_registry, "use_service_registry");
    can_use(yuan::plugin::PluginPermission::use_http_intercept, "use_http_intercept");
    can_use(yuan::plugin::PluginPermission::use_storage, "use_storage");

    // ---- 读取配置 ----
    if (context.config.loaded()) {
        enabled_ = context.config.get_bool("enabled", true);
        version_ = context.config.get_string("version", "n/a");
        std::cout << "config enabled=" << (enabled_ ? "true" : "false")
                  << " version=" << version_ << "\n";
    }

    // ---- 查询宿主服务 ----
    if (context.service_catalog) {
        const auto services = context.service_catalog->list_services();
        std::cout << "visible services:";
        for (const auto &service : services) {
            std::cout << " " << service.name
                      << "[" << service.contract_id
                      << " v" << service.contract_version << "]";
        }
        std::cout << "\n";
    }

    // ---- 日志输出 ----
    if (logger_) {
        logger_->log(
            yuan::plugin::HostLogLevel::info,
            __FILE__,
            __LINE__,
            __func__,
            "plugin attached to host app");
    }

    // ---- 定时调度: 使用 context.schedule_interval_task() 自动追踪 ----
    if (scheduler_) {
        helper.schedule_interval_task(
            std::chrono::seconds(10),
            [this]() {
                if (logger_) {
                    logger_->log(
                        yuan::plugin::HostLogLevel::debug,
                        __FILE__, __LINE__,
                        __func__,
                        "heartbeat tick from HelloWorld plugin");
                }
                std::cout << "[HelloWorld] heartbeat tick\n";
            },
            "HelloWorld.heartbeat");
        std::cout << "scheduled heartbeat task (auto-tracked)\n";

        // 演示一次性延迟任务: 3 秒后执行 (自动追踪)
        helper.schedule_task(
            std::chrono::seconds(3),
            []() {
                std::cout << "[HelloWorld] one-shot delayed task fired!\n";
            },
            "HelloWorld.delayed");
    }

    // ---- 注册插件服务 ----
    if (context.can_use(yuan::plugin::PluginPermission::use_service_registry)) {
        auto greeting = std::make_shared<GreetingService>();

        bool registered = helper.register_managed_service(
            "helloworld.greeting",
            std::move(greeting),
            kGreetingServiceContract,
            1);

        std::cout << "greeting service registered=" << (registered ? "true" : "false") << "\n";

        // 追踪服务注册
        if (resource_guard_ && registered) {
            auto *registry = service_registry_;
            auto pname = context.plugin_name;
            resource_guard_->track(pname, yuan::plugin::PluginResourceType::service_registration,
                [registry, pname]() {
                    if (registry) registry->unregister_plugin_services(pname);
                },
                "service:helloworld.greeting");
        }
    }

    // ---- HTTP 中间件注册演示 ----
    if (http_interceptor_ && http_interceptor_->is_available()) {
        auto mid_id = http_interceptor_->add_middleware(
            context.plugin_name,
            [](const std::string &path, const std::string &method,
               void * /*req*/, void * /*resp*/) -> bool {
                std::cout << "[HelloWorld] HTTP middleware: " << method << " " << path << "\n";
                return true; // 继续链路
            },
            "HelloWorld.logger");
        std::cout << "HTTP middleware registered, id=" << mid_id << "\n";

        // 注册一个路由
        auto route_id = http_interceptor_->add_route(
            context.plugin_name,
            "/hello",
            [](const std::string & /*path*/, const std::string & /*method*/,
               void *req, void *resp) {
                // 注意: 这里需要 cast 为 HttpRequest*/HttpResponse*
                // 类型擦除是为了避免核心层依赖协议层
                std::cout << "[HelloWorld] /hello route handler called\n";
                // 实际使用时: static_cast<HttpRequest*>(req), static_cast<HttpResponse*>(resp)
            });
        std::cout << "HTTP /hello route registered, id=" << route_id << "\n";
    }

    // ---- 事件订阅: 使用 context.subscribe_event() 自动追踪 ----
    helper.subscribe_event(yuan::app::events::application_started, [](const yuan::plugin::HostEvent &event) {
        try {
            const auto &appEvent = std::any_cast<const yuan::app::ApplicationEvent &>(event.payload);
            std::cout << "receive app started event: " << appEvent.app_name
                      << " workers=" << appEvent.worker_threads << "\n";
        } catch (const std::bad_any_cast &) {
            std::cout << "receive event with unexpected payload\n";
        }
    });

    helper.subscribe_event(yuan::plugin::events::plugin_loaded, [](const yuan::plugin::HostEvent &event) {
        try {
            const auto &pluginEvent = std::any_cast<const yuan::plugin::PluginEvent &>(event.payload);
            std::cout << "plugin loaded event: " << pluginEvent.plugin_name
                      << " app=" << pluginEvent.app_name << "\n";
        } catch (const std::bad_any_cast &) {
            std::cout << "receive plugin event with unexpected payload\n";
        }
    });

    // ---- 数据存储演示 ----
    if (storage_ && storage_->is_available()) {
        bool ok = storage_->set("demo_key", "Hello from HelloWorld plugin1111!");
        std::cout << "storage set demo_key=" << (ok ? "true" : "false") << "\n";

        auto val = storage_->get("demo_key");
        std::cout << "storage get demo_key=" << (val.has_value() ? val.value() : "<null>") << "\n";
    } else if (storage_) {
        std::cout << "storage backend not available (Redis not connected?)\n";
    }

    std::cout << "[HelloWorld] resource_guard tracked count: "
              << (resource_guard_ ? resource_guard_->tracked_count(context.plugin_name) : 0) << "\n";
    return true;
}

bool HelloWorldPlugin::on_health_check() const
{
    return enabled_;
}

void HelloWorldPlugin::on_config_changed(const yuan::plugin::PluginConfigView &config)
{
    if (config.loaded()) {
        enabled_ = config.get_bool("enabled", enabled_);
        version_ = config.get_string("version", version_);
        std::cout << "[HelloWorld] config hot-reloaded: enabled=" << (enabled_ ? "true" : "false")
                  << " version=" << version_ << "\n";

        if (logger_) {
            logger_->log(
                yuan::plugin::HostLogLevel::info,
                __FILE__, __LINE__,
                __func__,
                "configuration hot-reloaded");
        }
    }
}

void HelloWorldPlugin::on_release()
{
    if (service_registry_) {
        service_registry_->unregister_plugin_services("HelloWorld");
    }

    std::cout << "hello world on_release !!\n";
}

YUAN_API_C_EXPORT void * get_HelloWorld_plugin_instance()
{
    return new HelloWorldPlugin;
}
