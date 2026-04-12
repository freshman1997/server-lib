#ifndef __YUAN_PLUGIN_HOST_SERVICE_CATALOG_H__
#define __YUAN_PLUGIN_HOST_SERVICE_CATALOG_H__

#include <any>
#include <memory>
#include <string>
#include <vector>

namespace yuan::plugin
{

struct HostServiceDescriptor
{
    std::string name;
    std::string type_name;
    std::string contract_id;
    int contract_version = 1;
};

class HostServiceCatalog
{
public:
    virtual ~HostServiceCatalog() = default;

    virtual bool has_service(const std::string &name) const = 0;
    virtual std::vector<HostServiceDescriptor> list_services() const = 0;
    virtual std::any get_service(const std::string &name) const = 0;
    virtual bool describe_service(const std::string &name, HostServiceDescriptor &descriptor) const = 0;

    template <typename T>
    std::shared_ptr<T> get_service_as(const std::string &name) const
    {
        const auto service = get_service(name);
        if (!service.has_value()) {
            return {};
        }

        if (const auto exact = std::any_cast<std::shared_ptr<T>>(&service)) {
            return *exact;
        }

        return {};
    }
};

} // namespace yuan::plugin

#endif
