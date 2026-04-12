#ifndef __YUAN_APP_PLUGIN_SERVICE_CATALOG_H__
#define __YUAN_APP_PLUGIN_SERVICE_CATALOG_H__

#include "plugin/host_service_catalog.h"

#include <memory>
#include <string>

namespace yuan::app
{

class ServiceRegistry;

class PluginServiceCatalog : public plugin::HostServiceCatalog
{
public:
    explicit PluginServiceCatalog(std::shared_ptr<ServiceRegistry> registry);

    bool has_service(const std::string &name) const override;
    std::vector<plugin::HostServiceDescriptor> list_services() const override;
    std::any get_service(const std::string &name) const override;
    bool describe_service(const std::string &name, plugin::HostServiceDescriptor &descriptor) const override;

private:
    std::shared_ptr<ServiceRegistry> registry_;
};

} // namespace yuan::app

#endif
