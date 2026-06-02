#include "worker_plan.h"

#include <utility>

namespace yuan::app
{

namespace
{

std::size_t normalized_instance_count(const ServicePlacement &placement)
{
    return placement.instances == 0 ? 1 : placement.instances;
}

std::size_t estimate_dedicated_worker_count(const std::vector<ServiceDefinition> &definitions)
{
    std::size_t dedicated_worker_count = 0;
    for (const auto &definition : definitions) {
        const auto &placement = definition.descriptor.placement;
        if (placement.mode == PlacementMode::dedicated) {
            dedicated_worker_count += normalized_instance_count(placement);
        }
    }
    return dedicated_worker_count;
}

void assign_instance(WorkerPlan &worker,
                     const std::size_t service_index,
                     const std::size_t instance_index,
                     const std::size_t instance_count,
                     const ServiceDefinition &definition)
{
    worker.service_instances.push_back(ServiceInstancePlan{
        service_index,
        instance_index,
        instance_count,
        &definition
    });
}

} // namespace

std::vector<WorkerPlan> build_worker_plan(
    RuntimeWorkerConfig config,
    const std::vector<ServiceDefinition> &definitions)
{
    const auto data_worker_count = normalized_worker_count(config.worker_count);
    std::vector<WorkerPlan> workers;
    workers.reserve(data_worker_count + estimate_dedicated_worker_count(definitions));

    for (std::size_t i = 0; i < data_worker_count; ++i) {
        workers.push_back(WorkerPlan{i, data_worker_count, false, {}});
    }

    for (std::size_t service_index = 0; service_index < definitions.size(); ++service_index) {
        const auto &definition = definitions[service_index];
        const auto &placement = definition.descriptor.placement;

        switch (placement.mode) {
        case PlacementMode::disabled:
            break;
        case PlacementMode::singleton:
            assign_instance(workers.front(), service_index, 0, 1, definition);
            break;
        case PlacementMode::all_workers:
            for (std::size_t worker_index = 0; worker_index < data_worker_count; ++worker_index) {
                assign_instance(workers[worker_index], service_index, worker_index, data_worker_count, definition);
            }
            break;
        case PlacementMode::sharded: {
            const auto instance_count = normalized_instance_count(placement);
            for (std::size_t instance_index = 0; instance_index < instance_count; ++instance_index) {
                auto &worker = workers[instance_index % data_worker_count];
                assign_instance(worker, service_index, instance_index, instance_count, definition);
            }
            break;
        }
        case PlacementMode::dedicated: {
            const auto instance_count = normalized_instance_count(placement);
            for (std::size_t instance_index = 0; instance_index < instance_count; ++instance_index) {
                const auto worker_index = workers.size();
                WorkerPlan worker;
                worker.worker_index = worker_index;
                worker.dedicated = true;
                assign_instance(worker, service_index, instance_index, instance_count, definition);
                workers.push_back(std::move(worker));
            }
            break;
        }
        default:
            assign_instance(workers.front(), service_index, 0, 1, definition);
            break;
        }
    }

    const auto total_worker_count = workers.size();
    for (auto &worker : workers) {
        worker.worker_count = total_worker_count;
    }

    return workers;
}

} // namespace yuan::app
