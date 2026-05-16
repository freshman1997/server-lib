#include "endpoint_manager.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <utility>

namespace yuan::app
{

namespace
{

std::string normalize_host(std::string host)
{
    return host.empty() ? "0.0.0.0" : std::move(host);
}

std::string normalize_protocol(std::string protocol)
{
    if (protocol.empty()) {
        return "tcp";
    }

    std::transform(protocol.begin(), protocol.end(), protocol.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return protocol;
}

bool is_listening_endpoint(const ServiceEndpoint &endpoint)
{
    return endpoint.port >= 0 && normalize_protocol(endpoint.protocol) != "internal";
}

EndpointKey make_key(const ServiceEndpoint &endpoint)
{
    return EndpointKey{
        normalize_host(endpoint.host),
        endpoint.port,
        normalize_protocol(endpoint.protocol)
    };
}

std::string key_to_string(const EndpointKey &key)
{
    std::ostringstream out;
    out << key.protocol << "://" << key.host << ":" << key.port;
    return out.str();
}

bool same_logical_service(const EndpointBindingPlan &binding)
{
    if (binding.owners.empty()) {
        return true;
    }

    const auto &first = binding.owners.front().service_name;
    for (const auto &owner : binding.owners) {
        if (owner.service_name != first) {
            return false;
        }
    }
    return true;
}

bool has_multiple_workers(const EndpointBindingPlan &binding)
{
    std::set<std::size_t> workers;
    for (const auto &owner : binding.owners) {
        workers.insert(owner.worker_index);
    }
    return workers.size() > 1;
}

bool has_multiple_service_instances(const EndpointBindingPlan &binding)
{
    std::set<std::pair<std::size_t, std::size_t>> instances;
    for (const auto &owner : binding.owners) {
        instances.insert({ owner.service_index, owner.service_instance_index });
    }
    return instances.size() > 1;
}

EndpointBindingPlan *find_binding(std::vector<EndpointBindingPlan> &bindings, const EndpointKey &key)
{
    for (auto &binding : bindings) {
        if (binding.key == key) {
            return &binding;
        }
    }
    return nullptr;
}

void classify_binding(EndpointBindingPlan &binding)
{
    if (binding.owners.empty()) {
        binding.strategy = EndpointBindingStrategy::private_bind;
        return;
    }

    if (binding.key.port == 0) {
        binding.strategy = EndpointBindingStrategy::private_bind;
        return;
    }

    const bool replicated = has_multiple_workers(binding) || has_multiple_service_instances(binding);
    if (!replicated) {
        binding.strategy = EndpointBindingStrategy::private_bind;
        return;
    }

    if (same_logical_service(binding)) {
        binding.strategy = EndpointBindingStrategy::reuse_port;
        return;
    }

    binding.strategy = EndpointBindingStrategy::conflict;
    binding.diagnostic = "endpoint " + key_to_string(binding.key) +
                         " is claimed by multiple logical services";
}

} // namespace

const char *to_string(const EndpointBindingStrategy strategy) noexcept
{
    switch (strategy) {
    case EndpointBindingStrategy::private_bind:
        return "private_bind";
    case EndpointBindingStrategy::reuse_port:
        return "reuse_port";
    case EndpointBindingStrategy::conflict:
        return "conflict";
    default:
        return "unknown";
    }
}

bool operator==(const EndpointKey &left, const EndpointKey &right) noexcept
{
    return left.host == right.host &&
           left.port == right.port &&
           left.protocol == right.protocol;
}

bool EndpointBindingPlan::valid() const noexcept
{
    return strategy != EndpointBindingStrategy::conflict;
}

bool EndpointBindingPlan::requires_reuse_port() const noexcept
{
    return strategy == EndpointBindingStrategy::reuse_port;
}

bool EndpointPlan::valid() const noexcept
{
    for (const auto &binding : bindings) {
        if (!binding.valid()) {
            return false;
        }
    }
    return diagnostics.empty();
}

EndpointPlan EndpointManager::build_plan(const std::vector<WorkerPlan> &workers)
{
    EndpointPlan plan;

    for (const auto &worker : workers) {
        for (const auto &instance : worker.service_instances) {
            if (!instance.definition) {
                plan.diagnostics.push_back("worker plan contains service instance without definition");
                continue;
            }

            const auto &descriptor = instance.definition->descriptor;
            for (const auto &endpoint : descriptor.endpoints) {
                if (!is_listening_endpoint(endpoint)) {
                    continue;
                }

                auto key = make_key(endpoint);
                EndpointBindingPlan *binding = nullptr;
                if (key.port == 0) {
                    plan.bindings.push_back(EndpointBindingPlan{ key });
                    binding = &plan.bindings.back();
                } else {
                    binding = find_binding(plan.bindings, key);
                    if (!binding) {
                        plan.bindings.push_back(EndpointBindingPlan{ key });
                        binding = &plan.bindings.back();
                    }
                }

                binding->owners.push_back(EndpointOwner{
                    worker.worker_index,
                    instance.service_index,
                    instance.service_instance_index,
                    instance.service_instance_count,
                    descriptor.name,
                    endpoint.name
                });
            }
        }
    }

    for (auto &binding : plan.bindings) {
        classify_binding(binding);
        if (!binding.valid() && !binding.diagnostic.empty()) {
            plan.diagnostics.push_back(binding.diagnostic);
        }
    }

    return plan;
}

bool service_instance_requires_reuse_port(const ServiceInstancePlan &instance)
{
    if (!instance.definition || instance.service_instance_count <= 1) {
        return false;
    }

    for (const auto &endpoint : instance.definition->descriptor.endpoints) {
        if (is_listening_endpoint(endpoint) && endpoint.port > 0) {
            return true;
        }
    }
    return false;
}

} // namespace yuan::app
