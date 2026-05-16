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

        RuntimeContext make_service_context(const RuntimeContext &context, const ServiceInstanceEntry &entry)
        {
            auto service_context = context;
            service_context.active_service_name = entry.descriptor.name;
            service_context.service_index = entry.runtime.service_index;
            service_context.service_instance_index = entry.runtime.service_instance_index;
            service_context.service_instance_count = entry.runtime.service_instance_count == 0
                ? 1
                : entry.runtime.service_instance_count;
            service_context.listener_reuse_port = entry.runtime.listener_reuse_port;
            return service_context;
        }

        void fill_application_event(ApplicationEvent &event, const RuntimeContext &context)
        {
            event.app_name = context.app_name;
            event.run_mode = context.run_mode;
            event.worker_threads = context.worker_threads;
            event.runtime_worker_count = context.runtime_worker_count == 0
                ? context.worker_threads
                : context.runtime_worker_count;
            event.worker_index = context.worker_index;
            event.is_worker_process = context.is_worker_process;
            event.active_service_name = context.active_service_name;
            event.service_index = context.service_index;
            event.service_instance_index = context.service_instance_index;
            event.service_instance_count = context.service_instance_count == 0
                ? 1
                : context.service_instance_count;
            event.listener_reuse_port = context.listener_reuse_port;
        }

        ApplicationEvent make_application_event(const RuntimeContext &context)
        {
            ApplicationEvent event;
            fill_application_event(event, context);
            return event;
        }

        ServiceEvent make_service_event(const RuntimeContext &context, const std::string &service_name)
        {
            ServiceEvent event;
            fill_application_event(event, context);
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
        return add_service_instance(descriptor, std::move(service));
    }

    bool Application::add_service(const ServiceDescriptor & descriptor, std::shared_ptr<Service> service)
    {
        return add_service_instance(descriptor, std::move(service));
    }

    bool Application::add_service(ServiceDescriptor descriptor, ServiceFactory factory)
    {
        if (descriptor.name.empty() || !factory || descriptor.contract_version <= 0) {
            return false;
        }

        if (has_service_name(descriptor.name)) {
            return false;
        }

        service_definitions_.push_back(ServiceDefinition{ std::move(descriptor), std::move(factory) });
        return true;
    }

    bool Application::add_service_instance(const ServiceDescriptor & descriptor, std::shared_ptr<Service> service)
    {
        return add_service_instance(descriptor, std::move(service), {});
    }

    bool Application::add_service_instance(const ServiceDescriptor & descriptor,
                                           std::shared_ptr<Service> service,
                                           ServiceInstanceRuntime runtime)
    {
        if (descriptor.name.empty() || !service) {
            return false;
        }

        if (has_service_name(descriptor.name)) {
            return false;
        }

        if (!context_.service_registry) {
            context_.service_registry = std::make_shared<ServiceRegistry>();
        }
        if (!context_.service_registry->register_service(descriptor, service)) {
            return false;
        }
        service_instances_.push_back(ServiceEntry{ descriptor, std::move(service), runtime });
        return true;
    }

    const std::vector<ServiceEntry> &Application::services() const
    {
        return service_instances_;
    }

    const std::vector<ServiceDefinition> &Application::service_definitions() const
    {
        return service_definitions_;
    }

    const std::vector<ServiceInstanceEntry> &Application::service_instances() const
    {
        return service_instances_;
    }

    void Application::normalize_context()
    {
        switch (context_.run_mode) {
        case RunMode::single_thread:
            context_.worker_threads = 1;
            context_.runtime_worker_count = 1;
            context_.runtime_workers.worker_count = 1;
            break;
        case RunMode::multi_thread:
            if (context_.worker_threads < 2) {
                const auto hc = std::thread::hardware_concurrency();
                context_.worker_threads = hc > 1 ? hc : 2;
            }
            if (context_.runtime_workers.worker_count == 0) {
                context_.runtime_workers.worker_count = context_.worker_threads;
            }
            context_.runtime_worker_count = context_.runtime_workers.worker_count;
            break;
        case RunMode::multi_process:
            if (context_.worker_threads == 0) {
                context_.worker_threads = 1;
            }
            if (context_.runtime_workers.worker_count == 0) {
                context_.runtime_workers.worker_count = context_.worker_threads;
            }
            context_.runtime_worker_count = context_.runtime_workers.worker_count;
            break;
        default:
            context_.worker_threads = 1;
            context_.runtime_worker_count = 1;
            context_.runtime_workers.worker_count = 1;
            break;
        }
    }

    bool Application::has_service_name(const std::string &name) const
    {
        return has_service_instance(name) ||
               std::any_of(service_definitions_.begin(), service_definitions_.end(), [&](const ServiceDefinition &definition) {
                   return definition.descriptor.name == name;
               });
    }

    bool Application::has_service_instance(const std::string &name) const
    {
        return std::any_of(service_instances_.begin(), service_instances_.end(), [&](const ServiceEntry &entry) {
            return entry.descriptor.name == name;
        });
    }

    bool Application::materialize_service_definitions()
    {
        for (const auto &definition : service_definitions_) {
            if (has_service_instance(definition.descriptor.name)) {
                continue;
            }

            auto service = definition.create_instance();
            if (!service) {
                return false;
            }

            ServiceInstanceRuntime runtime;
            runtime.service_index = service_definitions_.empty()
                ? 0
                : static_cast<std::size_t>(&definition - service_definitions_.data());
            service_instances_.push_back(ServiceEntry{definition.descriptor, std::move(service), runtime});
        }
        return true;
    }

    bool Application::start_services_single_thread()
    {
        for (const auto &entry : service_instances_) {
            if (entry.service) {
                const auto service_context = make_service_context(context_, entry);
                entry.service->start();
                context_.event_bus->publish(events::service_started, make_service_event(service_context, entry.descriptor.name));
            }
        }

        return true;
    }

    bool Application::start_services_multi_thread()
    {
        std::mutex mutex;
        std::exception_ptr first_exception;
        std::vector<std::thread> workers;
        workers.reserve(service_instances_.size());

        for (const auto &entry : service_instances_) {
            auto service_context = make_service_context(context_, entry);
            auto event_bus = context_.event_bus;
            workers.emplace_back([
                &,
                service = entry.service,
                name = entry.descriptor.name,
                service_context,
                event_bus
            ]() {
            if (!service) {
                return;
            }

            try {
                service->start();
                if (event_bus) {
                    event_bus->publish(events::service_started, make_service_event(service_context, name));
                }
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

        if (!materialize_service_definitions()) {
            return false;
        }

        for (const auto &entry : service_instances_) {
            if (entry.service) {
                context_.service_registry->register_service(entry.descriptor, entry.service);
            }
        }

        for (const auto &entry : service_instances_) {
            if (!entry.service) {
                return false;
            }

            auto service_context = make_service_context(context_, entry);

            if (auto *contextAware = dynamic_cast<RuntimeContextAwareService *>(&*entry.service)) {
                contextAware->set_runtime_context(service_context);
            }

            if (!entry.service->init()) {
                return false;
            }

            context_.event_bus->publish(events::service_initialized, make_service_event(service_context, entry.descriptor.name));
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

        for (auto it = service_instances_.rbegin(); it != service_instances_.rend(); ++it) {
            if (it->service) {
                it->service->stop();
                if (context_.event_bus) {
                    const auto service_context = make_service_context(context_, *it);
                    context_.event_bus->publish(events::service_stopped, make_service_event(service_context, it->descriptor.name));
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
