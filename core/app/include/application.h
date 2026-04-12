#ifndef __YUAN_APP_APPLICATION_H__
#define __YUAN_APP_APPLICATION_H__

#include "runtime_context.h"
#include "runtime_plan.h"
#include "service.h"
#include "service_registry.h"

#include <algorithm>
#include <type_traits>
#include <typeinfo>
#include <memory>
#include <string>
#include <vector>

namespace yuan::app
{

struct ServiceEntry
{
    ServiceDescriptor descriptor;
    std::shared_ptr<Service> service;
};

class Application
{
public:
    explicit Application(RuntimeContext context = {});

    void set_context(RuntimeContext context);
    const RuntimeContext& context() const;
    RuntimePlan runtime_plan() const;

    bool add_service(const std::string& name, std::shared_ptr<Service> service);
    bool add_service(const ServiceDescriptor &descriptor, std::shared_ptr<Service> service);

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

        const auto it = std::find_if(services_.begin(), services_.end(), [&](const ServiceEntry &entry) {
            return entry.descriptor.name == name;
        });
        if (it != services_.end()) {
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

        services_.push_back(ServiceEntry{descriptor, std::move(service)});
        return true;
    }

    const std::vector<ServiceEntry>& services() const;

    bool init();
    bool start();
    void stop();

    bool is_initialized() const;
    bool is_running() const;

private:
    void normalize_context();
    bool start_services_single_thread();
    bool start_services_multi_thread();
    RuntimeContext context_;
    std::vector<ServiceEntry> services_;
    bool initialized_ = false;
    bool running_ = false;
};

} // namespace yuan::app

#endif
