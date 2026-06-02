#ifndef __YUAN_APP_APPLICATION_H__
#define __YUAN_APP_APPLICATION_H__

#include "runtime_context.h"
#include "runtime_plan.h"
#include "service_definition.h"

#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>
#include <vector>

namespace yuan::app
{

class Application
{
public:
    explicit Application(RuntimeContext context = {});

    void set_context(RuntimeContext context);
    const RuntimeContext& context() const;
    RuntimePlan runtime_plan() const;

    bool add_service(const std::string& name, std::shared_ptr<Service> service);
    bool add_service(const ServiceDescriptor &descriptor, std::shared_ptr<Service> service);
    bool add_service(ServiceDescriptor descriptor, ServiceFactory factory);
    bool add_service_instance(const ServiceDescriptor &descriptor, std::shared_ptr<Service> service);
    bool add_service_instance(const ServiceDescriptor &descriptor,
                              std::shared_ptr<Service> service,
                              ServiceInstanceRuntime runtime);

    template <typename T>
    bool add_typed_service(const std::string &name,
                           std::shared_ptr<T> service,
                           const std::string &contract_id = {},
                           int contract_version = 1)
    {
        static_assert(std::is_base_of_v<Service, T>, "T must derive from Service");
        if (name.empty() || !service || contract_version <= 0) {
            return false;
        }

        if (has_service_name(name)) {
            return false;
        }

        ServiceDescriptor descriptor;
        descriptor.name = name;
        descriptor.type_name = typeid(T).name();
        descriptor.contract_id = contract_id.empty() ? name : contract_id;
        descriptor.contract_version = contract_version;

        if (!context_.service_registry) {
            context_.service_registry = std::make_shared<ServiceRegistry>();
        }
        if (!context_.service_registry->register_typed_service<T>(
                name,
                service,
                descriptor.contract_id,
                descriptor.contract_version)) {
            return false;
        }

        service_instances_.push_back(ServiceEntry{descriptor, std::move(service), {}});
        service_names_.insert(service_instances_.back().descriptor.name);
        service_instance_names_.insert(service_instances_.back().descriptor.name);
        return true;
    }

    const std::vector<ServiceEntry>& services() const;
    const std::vector<ServiceDefinition>& service_definitions() const;
    const std::vector<ServiceInstanceEntry>& service_instances() const;

    bool init();
    bool start();
    void stop();

    bool is_initialized() const;
    bool is_running() const;

private:
    bool has_service_name(const std::string &name) const;
    bool has_service_instance(const std::string &name) const;
    bool materialize_service_definitions();
    bool start_services_single_thread();
    bool start_services_multi_thread();
    RuntimeContext context_;
    std::vector<ServiceDefinition> service_definitions_;
    std::vector<ServiceInstanceEntry> service_instances_;
    std::unordered_set<std::string> service_names_;
    std::unordered_set<std::string> service_instance_names_;
    bool initialized_ = false;
    bool running_ = false;
};

} // namespace yuan::app

#endif
