#include "service_registry.h"

#include "service.h"

namespace yuan::app
{

namespace
{

ServiceDescriptor normalize_descriptor(ServiceDescriptor descriptor, const std::shared_ptr<Service> &service)
{
    if (!service) {
        return descriptor;
    }

    if (descriptor.type_name.empty()) {
        descriptor.type_name = typeid(*service).name();
    }
    if (descriptor.contract_id.empty()) {
        descriptor.contract_id = descriptor.name;
    }
    return descriptor;
}

bool is_valid_descriptor(const ServiceDescriptor &descriptor)
{
    return !descriptor.name.empty() &&
           !descriptor.type_name.empty() &&
           !descriptor.contract_id.empty() &&
           descriptor.contract_version > 0;
}

} // namespace

bool ServiceRegistry::register_service(const std::string &name, const std::shared_ptr<Service> &service)
{
    ServiceDescriptor descriptor;
    descriptor.name = name;
    return register_service(descriptor, service);
}

bool ServiceRegistry::register_service(const ServiceDescriptor &descriptor, const std::shared_ptr<Service> &service)
{
    auto normalized = normalize_descriptor(descriptor, service);
    if (!service || !is_valid_descriptor(normalized)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    services_[normalized.name] = ServiceEntry{std::move(normalized), service};
    return true;
}

void ServiceRegistry::unregister_service(const std::string &name)
{
    if (name.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    services_.erase(name);
}

std::shared_ptr<Service> ServiceRegistry::find_service(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(name);
    return it == services_.end() ? nullptr : it->second.service;
}

std::any ServiceRegistry::find_typed_service(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(name);
    if (it == services_.end()) {
        return {};
    }
    return it->second.typed_instance;
}

bool ServiceRegistry::describe_service(const std::string &name, ServiceDescriptor &descriptor) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(name);
    if (it == services_.end() || !it->second.service) {
        return false;
    }

    descriptor = it->second.descriptor;
    return true;
}

std::vector<std::string> ServiceRegistry::list_services() const
{
    std::vector<std::string> names;

    std::lock_guard<std::mutex> lock(mutex_);
    names.reserve(services_.size());
    for (const auto &[name, entry] : services_) {
        if (entry.service) {
            names.push_back(name);
        }
    }

    return names;
}

std::vector<ServiceDescriptor> ServiceRegistry::list_descriptors() const
{
    std::vector<ServiceDescriptor> descriptors;

    std::lock_guard<std::mutex> lock(mutex_);
    descriptors.reserve(services_.size());
    for (const auto &[_, entry] : services_) {
        if (entry.service) {
            descriptors.push_back(entry.descriptor);
        }
    }

    return descriptors;
}

} // namespace yuan::app
