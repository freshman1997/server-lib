#ifndef __YUAN_APP_SERVICE_DEFINITION_H__
#define __YUAN_APP_SERVICE_DEFINITION_H__

#include "service.h"
#include "service_registry.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace yuan::app
{

struct ServiceInstanceRuntime
{
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;
    bool listener_reuse_port = false;
};

using ServiceFactory = std::function<std::shared_ptr<Service>()>;

struct ServiceDefinition
{
    ServiceDescriptor descriptor;
    ServiceFactory factory;

    std::shared_ptr<Service> create_instance() const
    {
        return factory ? factory() : nullptr;
    }
};

struct ServiceEntry
{
    ServiceDescriptor descriptor;
    std::shared_ptr<Service> service;
    ServiceInstanceRuntime runtime;
};

using ServiceInstanceEntry = ServiceEntry;

} // namespace yuan::app

#endif
