#include "application.h"

#include "app_events.h"
#include "eventbus/event_bus.h"
#include "eventbus/event_type_registry.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace yuan::app
{

    namespace
    {

        ApplicationEvent make_application_event(const RuntimeContext &context)
        {
            return ApplicationEvent{
                context.app_name,
                context.run_mode,
                context.worker_threads,
                context.worker_index,
                context.is_worker_process
            };
        }

        ServiceEvent make_service_event(const RuntimeContext &context, const std::string &service_name)
        {
            ServiceEvent event;
            event.app_name = context.app_name;
            event.run_mode = context.run_mode;
            event.worker_threads = context.worker_threads;
            event.worker_index = context.worker_index;
            event.is_worker_process = context.is_worker_process;
            event.service_name = service_name;
            return event;
        }

        void register_builtin_event_types(eventbus::EventTypeRegistry &registry)
        {
            using namespace eventbus;

            registry.register_type(events::application_initialized, "lifecycle", "ApplicationEvent",
                                   "Application finished initialization", EventScope::global);
            registry.register_type(events::application_started, "lifecycle", "ApplicationEvent",
                                   "Application started", EventScope::global);
            registry.register_type(events::application_stopping, "lifecycle", "ApplicationEvent",
                                   "Application stopping", EventScope::global);
            registry.register_type(events::service_initialized, "lifecycle", "ServiceEvent",
                                   "Service finished initialization", EventScope::global);
            registry.register_type(events::service_started, "lifecycle", "ServiceEvent",
                                   "Service started", EventScope::global);
            registry.register_type(events::service_stopped, "lifecycle", "ServiceEvent",
                                   "Service stopped", EventScope::global);
        }

    } // namespace

    Application::Application(RuntimeContext context)
        : context_(std::move(context))
    {
        normalize_context();
    }

    void Application::set_context(RuntimeContext context)
    {
        context_ = std::move(context);
        normalize_context();
    }

    const RuntimeContext &Application::context() const
    {
        return context_;
    }

    RuntimePlan Application::runtime_plan() const
    {
        return derive_runtime_plan(context_);
    }

    bool Application::add_service(const std::string & name, std::shared_ptr<Service> service)
    {
        ServiceDescriptor descriptor;
        descriptor.name = name;
        return add_service(descriptor, std::move(service));
    }

    bool Application::add_service(const ServiceDescriptor & descriptor, std::shared_ptr<Service> service)
    {
        if (descriptor.name.empty() || !service) {
            return false;
        }

        const auto it = std::find_if(services_.begin(), services_.end(), [&](const ServiceEntry &entry) {
        return entry.descriptor.name == descriptor.name;
        });
        if (it != services_.end()) {
            return false;
        }

        if (!context_.service_registry) {
            context_.service_registry = std::make_shared<ServiceRegistry>();
        }
        if (!context_.service_registry->register_service(descriptor, service)) {
            return false;
        }
        services_.push_back(ServiceEntry{ descriptor, std::move(service) });
        return true;
    }

    const std::vector<ServiceEntry> &Application::services() const
    {
        return services_;
    }

    void Application::normalize_context()
    {
        switch (context_.run_mode) {
        case RunMode::single_thread:
            context_.worker_threads = 1;
            break;
        case RunMode::multi_thread:
            if (context_.worker_threads < 2) {
                const auto hc = std::thread::hardware_concurrency();
                context_.worker_threads = hc > 1 ? hc : 2;
            }
            break;
        case RunMode::multi_process:
            if (context_.worker_threads == 0) {
                context_.worker_threads = 1;
            }
            break;
        default:
            context_.worker_threads = 1;
            break;
        }
    }

    bool Application::start_services_single_thread()
    {
        for (const auto &entry : services_) {
            if (entry.service) {
                entry.service->start();
                context_.event_bus->publish(events::service_started, make_service_event(context_, entry.descriptor.name));
            }
        }

        return true;
    }

    bool Application::start_services_multi_thread()
    {
        std::mutex mutex;
        std::exception_ptr first_exception;
        std::vector<std::thread> workers;
        workers.reserve(services_.size());

        for (const auto &entry : services_) {
            workers.emplace_back([
                &,
                service = entry.service,
                name = entry.descriptor.name
            ]() {
            if (!service) {
                return;
            }

            try {
                service->start();
                context_.event_bus->publish(events::service_started, make_service_event(context_, name));
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!first_exception) {
                    first_exception = std::current_exception();
                }
            }
             });
        }

        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        if (first_exception) {
            try
            {
                std::rethrow_exception(first_exception);
            }
            catch (...)
            {
                stop();
                return false;
            }
        }

        return true;
    }

    bool Application::init()
    {
        if (initialized_) {
            return true;
        }

        if (!context_.event_bus) {
            context_.event_bus = std::make_shared<yuan::eventbus::EventBus>();
        }
        if (!context_.service_registry) {
            context_.service_registry = std::make_shared<ServiceRegistry>();
        }
        if (!context_.event_type_registry) {
            context_.event_type_registry = std::make_shared<yuan::eventbus::EventTypeRegistry>();
        }
        for (const auto &entry : services_) {
            if (entry.service) {
                context_.service_registry->register_service(entry.descriptor, entry.service);
            }
        }

        for (const auto &entry : services_) {
            if (auto *contextAware = dynamic_cast<RuntimeContextAwareService *>(entry.service.get())) {
                contextAware->set_runtime_context(context_);
            }

            if (!entry.service || !entry.service->init()) {
                return false;
            }

            context_.event_bus->publish(events::service_initialized, make_service_event(context_, entry.descriptor.name));
        }

        initialized_ = true;
        register_builtin_event_types(*context_.event_type_registry);
        context_.event_bus->publish(events::application_initialized, make_application_event(context_));
        return true;
    }

    bool Application::start()
    {
        if (running_) {
            return true;
        }

        if (!init()) {
            return false;
        }

        const auto plan = runtime_plan();
        if (!plan.implemented) {
            return false;
        }

        if (plan.parallel_service_start) {
            if (!start_services_multi_thread()) {
                return false;
            }
        } else {
            if (!start_services_single_thread()) {
                return false;
            }
        }

        running_ = true;
        context_.event_bus->publish(events::application_started, make_application_event(context_));
        return true;
    }

    void Application::stop()
    {
        if (!initialized_ && !running_) {
            return;
        }

        if (context_.event_bus) {
            context_.event_bus->publish(events::application_stopping, make_application_event(context_));
        }

        for (auto it = services_.rbegin(); it != services_.rend(); ++it) {
            if (it->service) {
                it->service->stop();
                if (context_.event_bus) {
                    context_.event_bus->publish(events::service_stopped, make_service_event(context_, it->descriptor.name));
                }
                if (context_.service_registry) {
                    context_.service_registry->unregister_service(it->descriptor.name);
                }
            }
        }

        running_ = false;
        initialized_ = false;
    }

    bool Application::is_initialized() const
    {
        return initialized_;
    }

    bool Application::is_running() const
    {
        return running_;
    }

} // namespace yuan::app
