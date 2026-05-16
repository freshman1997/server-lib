#ifndef __YUAN_APP_WORKER_PLAN_H__
#define __YUAN_APP_WORKER_PLAN_H__

#include "application.h"
#include "runtime_context.h"

#include <cstddef>
#include <string>
#include <vector>

namespace yuan::app
{

struct RuntimeIdentity
{
    std::size_t worker_index = 0;
    std::size_t worker_count = 1;
    bool is_worker_process = false;

    std::string active_service_name;
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;
};

struct ServiceInstancePlan
{
    std::size_t service_index = 0;
    std::size_t service_instance_index = 0;
    std::size_t service_instance_count = 1;
    const ServiceDefinition *definition = nullptr;
};

struct WorkerPlan
{
    std::size_t worker_index = 0;
    std::size_t worker_count = 1;
    bool dedicated = false;
    std::vector<ServiceInstancePlan> service_instances;
};

std::vector<WorkerPlan> build_worker_plan(
    RuntimeWorkerConfig config,
    const std::vector<ServiceDefinition> &definitions);

} // namespace yuan::app

#endif
