#include "worker_plan.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{

class DummyService final : public yuan::app::Service
{
public:
    bool init() override { return true; }
    void start() override {}
    void stop() override {}
};

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

yuan::app::ServiceDefinition definition(std::string name, yuan::app::PlacementMode mode, std::size_t instances = 1)
{
    yuan::app::ServiceDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.contract_id = descriptor.name;
    descriptor.contract_version = 1;
    descriptor.placement.mode = mode;
    descriptor.placement.instances = instances;

    return yuan::app::ServiceDefinition{
        std::move(descriptor),
        [] {
            return std::make_shared<DummyService>();
        }
    };
}

std::size_t count_service_instances(const std::vector<yuan::app::WorkerPlan> &workers, const std::string &name)
{
    std::size_t count = 0;
    for (const auto &worker : workers) {
        for (const auto &instance : worker.service_instances) {
            if (instance.definition && instance.definition->descriptor.name == name) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

int main()
{
    yuan::app::RuntimeWorkerConfig config;
    config.worker_count = 4;

    std::vector<yuan::app::ServiceDefinition> definitions;
    definitions.push_back(definition("http", yuan::app::PlacementMode::all_workers));
    definitions.push_back(definition("mqtt", yuan::app::PlacementMode::sharded, 2));
    definitions.push_back(definition("nas", yuan::app::PlacementMode::singleton));
    definitions.push_back(definition("bt", yuan::app::PlacementMode::disabled));
    definitions.push_back(definition("admin", yuan::app::PlacementMode::dedicated, 1));

    const auto workers = yuan::app::build_worker_plan(config, definitions);

    if (!require(workers.size() == 5, "four data workers plus one dedicated worker should be planned")) {
        return 1;
    }

    if (!require(count_service_instances(workers, "http") == 4, "http all_workers should create one instance per data worker")) {
        return 1;
    }

    if (!require(count_service_instances(workers, "mqtt") == 2, "mqtt sharded(2) should create two instances")) {
        return 1;
    }

    if (!require(count_service_instances(workers, "nas") == 1, "nas singleton should create one instance")) {
        return 1;
    }

    if (!require(count_service_instances(workers, "bt") == 0, "disabled service should create no instance")) {
        return 1;
    }

    if (!require(count_service_instances(workers, "admin") == 1, "dedicated admin should create one instance")) {
        return 1;
    }

    if (!require(workers[0].service_instances.size() == 3, "worker 0 should share http, mqtt, and nas")) {
        return 1;
    }

    if (!require(workers[1].service_instances.size() == 2, "worker 1 should share http and mqtt")) {
        return 1;
    }

    if (!require(workers[4].dedicated, "last worker should be dedicated")) {
        return 1;
    }

    if (!require(!workers[0].dedicated, "data workers should not be marked dedicated")) {
        return 1;
    }

    if (!require(workers[0].service_instances[0].service_instance_index == 0 &&
                 workers[3].service_instances[0].service_instance_index == 3,
                 "all_workers instances should use worker index as instance index")) {
        return 1;
    }

    if (!require(workers[0].service_instances[0].service_instance_count == 4,
                 "all_workers service instance count should equal data worker count")) {
        return 1;
    }

    for (const auto &worker : workers) {
        if (!require(worker.worker_count == workers.size(), "each worker should know total worker count")) {
            return 1;
        }
    }

    yuan::app::RuntimeWorkerConfig zero_worker_config;
    zero_worker_config.worker_count = 0;
    const auto normalized_workers = yuan::app::build_worker_plan(
        zero_worker_config,
        {definition("zero-shard", yuan::app::PlacementMode::sharded, 0)});
    if (!require(normalized_workers.size() == 1,
                 "zero worker count should normalize to one worker")) {
        return 1;
    }
    if (!require(normalized_workers[0].service_instances.size() == 1 &&
                 normalized_workers[0].service_instances[0].service_instance_count == 1,
                 "zero sharded instance count should normalize to one instance")) {
        return 1;
    }

    yuan::app::RuntimeWorkerConfig dedicated_config;
    dedicated_config.worker_count = 2;
    const auto dedicated_workers = yuan::app::build_worker_plan(
        dedicated_config,
        {definition("dedicated-pair", yuan::app::PlacementMode::dedicated, 2)});
    if (!require(dedicated_workers.size() == 4,
                 "two data workers plus two dedicated workers should be planned")) {
        return 1;
    }
    if (!require(dedicated_workers[2].dedicated && dedicated_workers[3].dedicated,
                 "dedicated instances should allocate dedicated workers")) {
        return 1;
    }
    if (!require(dedicated_workers[2].service_instances[0].service_instance_index == 0 &&
                 dedicated_workers[3].service_instances[0].service_instance_index == 1 &&
                 dedicated_workers[2].service_instances[0].service_instance_count == 2,
                 "dedicated instances should preserve shard identity")) {
        return 1;
    }

    std::cout << "worker plan test passed\n";
    return 0;
}
