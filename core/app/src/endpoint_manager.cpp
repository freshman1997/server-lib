#include "endpoint_manager.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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

struct InstanceKey
{
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
};

bool operator==(const InstanceKey &left, const InstanceKey &right) noexcept
{
    return left.service_index == right.service_index &&
           left.service_instance_index == right.service_instance_index;
}

struct InstanceKeyHash
{
    std::size_t operator()(const InstanceKey &key) const noexcept
    {
        const auto service_hash = std::hash<std::size_t>{}(key.service_index);
        const auto instance_hash = std::hash<std::size_t>{}(key.service_instance_index);
        return service_hash ^ (instance_hash + 0x9e3779b97f4a7c15ULL + (service_hash << 6U) + (service_hash >> 2U));
    }
};

struct BindingClassification
{
    bool replicated = false;
    bool same_logical_service = true;
};

BindingClassification analyze_binding(const EndpointBindingPlan &binding)
{
    BindingClassification result;
    if (binding.owners.empty()) {
        return result;
    }

    const auto &first_service = binding.owners.front().service_name;
    std::unordered_set<std::size_t> workers;
    std::unordered_set<InstanceKey, InstanceKeyHash> instances;
    workers.reserve(binding.owners.size());
    instances.reserve(binding.owners.size());

    for (const auto &owner : binding.owners) {
        if (owner.service_name != first_service) {
            result.same_logical_service = false;
        }
        workers.insert(owner.worker_index);
        instances.insert({ owner.service_index, owner.service_instance_index });
        result.replicated = workers.size() > 1 || instances.size() > 1;
    }

    return result;
}

struct EndpointKeyHash
{
    std::size_t operator()(const EndpointKey &key) const noexcept
    {
        const auto host_hash = std::hash<std::string>{}(key.host);
        const auto port_hash = std::hash<int>{}(key.port);
        const auto protocol_hash = std::hash<std::string>{}(key.protocol);
        return host_hash ^ (port_hash + 0x9e3779b97f4a7c15ULL + (host_hash << 6U) + (host_hash >> 2U)) ^
               (protocol_hash + 0x9e3779b97f4a7c15ULL + (port_hash << 6U) + (port_hash >> 2U));
    }
};

using BindingIndex = std::unordered_map<EndpointKey, std::size_t, EndpointKeyHash>;

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

    const auto classification = analyze_binding(binding);
    if (!classification.replicated) {
        binding.strategy = EndpointBindingStrategy::private_bind;
        return;
    }

    if (classification.same_logical_service) {
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
    BindingIndex fixed_bindings;

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
                std::size_t binding_index = 0;
                if (key.port == 0) {
                    plan.bindings.push_back(EndpointBindingPlan{ key });
                    binding_index = plan.bindings.size() - 1;
                } else {
                    const auto existing = fixed_bindings.find(key);
                    if (existing == fixed_bindings.end()) {
                        plan.bindings.push_back(EndpointBindingPlan{ key });
                        binding_index = plan.bindings.size() - 1;
                        fixed_bindings.emplace(plan.bindings.back().key, binding_index);
                    } else {
                        binding_index = existing->second;
                    }
                }

                plan.bindings[binding_index].owners.push_back(EndpointOwner{
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

bool service_instance_requires_reuse_port(const EndpointPlan &plan, const ServiceInstancePlan &instance)
{
    if (!instance.definition) {
        return false;
    }

    const auto &service_name = instance.definition->descriptor.name;
    for (const auto &binding : plan.bindings) {
        if (!binding.requires_reuse_port()) {
            continue;
        }

        for (const auto &owner : binding.owners) {
            if (owner.service_index == instance.service_index &&
                owner.service_instance_index == instance.service_instance_index &&
                owner.service_name == service_name) {
                return true;
            }
        }
    }

    return false;
}

} // namespace yuan::app
