#include "server_runtime_host.h"
#include "eventbus/event_bus.h"

namespace yuan::server
{

    ServerRuntimeHost::ServerRuntimeHost(Config config)
        : config_(std::move(config))
    {
    }

    ServerRuntimeHost::~ServerRuntimeHost()
    {
        stop();
    }

    void ServerRuntimeHost::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        runtime_context_ = context;
    }

    bool ServerRuntimeHost::start(std::function<void()> serve_fn)
    {
        if (started_.exchange(true)) {
            return false;
        }

        publish(events::service_activating);

        worker_ = std::thread([fn = std::move(serve_fn), this]() {
            fn();
            started_.store(false);
        });

        publish(events::service_activated);
        return true;
    }

    bool ServerRuntimeHost::start_inline(std::function<void()> start_fn)
    {
        if (started_.exchange(true)) {
            return false;
        }

        publish(events::service_activating);

        try
        {
            if (start_fn) {
                start_fn();
            }
        }
        catch (...)
        {
            started_.store(false);
            throw;
        }

        publish(events::service_activated);
        return true;
    }

    void ServerRuntimeHost::stop(std::function<void()> stop_fn)
    {
        publish(events::service_stopping);

        if (stop_fn) {
            stop_fn();
        }

        started_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }

        publish(events::service_stopped);
    }

    bool ServerRuntimeHost::is_started() const
    {
        return started_.load();
    }

    void ServerRuntimeHost::publish_custom(const std::string & event_name, std::any payload)
    {
        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(event_name, std::move(payload));
        }
    }

    ServiceRuntimeEvent ServerRuntimeHost::make_event() const
    {
        ServiceRuntimeEvent event;
        event.app_name = runtime_context_.app_name;
        event.run_mode = runtime_context_.run_mode;
        event.worker_threads = runtime_context_.worker_threads;
        event.runtime_worker_count = runtime_context_.runtime_worker_count == 0
            ? runtime_context_.worker_threads
            : runtime_context_.runtime_worker_count;
        event.worker_index = runtime_context_.worker_index;
        event.is_worker_process = runtime_context_.is_worker_process;
        event.active_service_name = runtime_context_.active_service_name;
        event.service_index = runtime_context_.service_index;
        event.service_instance_index = runtime_context_.service_instance_index;
        event.service_instance_count = runtime_context_.service_instance_count == 0
            ? 1
            : runtime_context_.service_instance_count;
        event.listener_reuse_port = runtime_context_.listener_reuse_port;
        event.service_name = config_.service_name;
        event.protocol = config_.protocol;
        event.port = config_.port;
        return event;
    }

    void ServerRuntimeHost::publish(const char * event_type)
    {
        if (runtime_context_.event_bus) {
            runtime_context_.event_bus->publish(event_type, make_event());
        }
    }

} // namespace yuan::server
