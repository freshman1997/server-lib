#ifndef __YUAN_APP_ENDPOINT_MANAGER_H__
#define __YUAN_APP_ENDPOINT_MANAGER_H__

#include "worker_plan.h"

#include <cstddef>
#include <string>
#include <vector>

namespace yuan::app
{

enum class EndpointBindingStrategy
{
    private_bind,
    reuse_port,
    conflict,
};

const char *to_string(EndpointBindingStrategy strategy) noexcept;

struct EndpointKey
{
    std::string host = "0.0.0.0";
    int port = 0;
    std::string protocol = "tcp";
};

bool operator==(const EndpointKey &left, const EndpointKey &right) noexcept;

struct EndpointOwner
{
    std::size_t worker_index = 0;
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;
    std::string service_name;
    std::string endpoint_name;
};

struct EndpointBindingPlan
{
    EndpointKey key;
    EndpointBindingStrategy strategy = EndpointBindingStrategy::private_bind;
    std::vector<EndpointOwner> owners;
    std::string diagnostic;

    bool valid() const noexcept;
    bool requires_reuse_port() const noexcept;
};

struct EndpointPlan
{
    std::vector<EndpointBindingPlan> bindings;
    std::vector<std::string> diagnostics;

    bool valid() const noexcept;
};

class EndpointManager
{
public:
    static EndpointPlan build_plan(const std::vector<WorkerPlan> &workers);
};

bool service_instance_requires_reuse_port(const ServiceInstancePlan &instance);

} // namespace yuan::app

#endif
