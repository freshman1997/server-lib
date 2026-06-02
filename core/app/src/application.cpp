#include "application.h"

#include "app_events.h"
#include "eventbus/event_bus.h"
#include "eventbus/event_type_registry.h"

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
        normalize_runtime_context(context_);
    }

    void Application::set_context(RuntimeContext context)
    {
        context_ = std::move(context);
        normalize_runtime_context(context_);
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
        service_names_.insert(service_definitions_.back().descriptor.name);
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
        service_names_.insert(service_instances_.back().descriptor.name);
        service_instance_names_.insert(service_instances_.back().descriptor.name);
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

    bool Application::has_service_name(const std::string &name) const
    {
        return service_names_.find(name) != service_names_.end();
    }

    bool Application::has_service_instance(const std::string &name) const
    {
        return service_instance_names_.find(name) != service_instance_names_.end();
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
            service_instance_names_.insert(service_instances_.back().descriptor.name);
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
