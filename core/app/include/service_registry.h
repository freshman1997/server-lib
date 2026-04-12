#ifndef __YUAN_APP_SERVICE_REGISTRY_H__
#define __YUAN_APP_SERVICE_REGISTRY_H__

#include <any>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace yuan::app
{

class Service;

struct ServiceDescriptor
{
    std::string name;
    std::string type_name;
    std::string contract_id;
    int contract_version = 1;
};

class ServiceRegistry
{
public:
    bool register_service(const std::string &name, const std::shared_ptr<Service> &service);
    bool register_service(const ServiceDescriptor &descriptor, const std::shared_ptr<Service> &service);
    void unregister_service(const std::string &name);

    std::shared_ptr<Service> find_service(const std::string &name) const;
    std::any find_typed_service(const std::string &name) const;
    bool describe_service(const std::string &name, ServiceDescriptor &descriptor) const;
    std::vector<std::string> list_services() const;
    std::vector<ServiceDescriptor> list_descriptors() const;

    template <typename T>
    bool register_typed_service(const std::string &name,
                                std::shared_ptr<T> service,
                                const std::string &contract_id = {},
                                int contract_version = 1)
    {
        static_assert(std::is_base_of_v<Service, T>, "T must derive from Service");
        if (!service || name.empty() || contract_version <= 0) {
            return false;
        }

        ServiceDescriptor descriptor;
        descriptor.name = name;
        descriptor.type_name = typeid(T).name();
        descriptor.contract_id = contract_id.empty() ? name : contract_id;
        descriptor.contract_version = contract_version;

        std::lock_guard<std::mutex> lock(mutex_);
        services_[name] = ServiceEntry{std::move(descriptor), service, std::move(service)};
        return true;
    }

    template <typename T>
    std::shared_ptr<T> find_service_as(const std::string &name) const
    {
        static_assert(std::is_base_of_v<Service, T>, "T must derive from Service");

        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = services_.find(name);
        if (it == services_.end() || !it->second.service) {
            return {};
        }

        if (const auto exact = std::any_cast<std::shared_ptr<T>>(&it->second.typed_instance)) {
            return *exact;
        }

        return std::dynamic_pointer_cast<T>(it->second.service);
    }

private:
    struct ServiceEntry
    {
        ServiceDescriptor descriptor;
        std::shared_ptr<Service> service;
        std::any typed_instance;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ServiceEntry> services_;
};

} // namespace yuan::app

#endif
