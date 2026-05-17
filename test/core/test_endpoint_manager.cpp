#include "bootstrap.h"
#include "endpoint_manager.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    void require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }

    class DummyService final : public yuan::app::Service
    {
    public:
        bool init() override
        {
            return true;
        }

        void start() override
        {
        }

        void stop() override
        {
        }
    };

    yuan::app::ServiceDefinition make_definition(
        std::string name,
        yuan::app::PlacementMode mode,
        std::vector<yuan::app::ServiceEndpoint> endpoints,
        std::size_t instances = 1)
    {
        yuan::app::ServiceDescriptor descriptor;
        descriptor.name = std::move(name);
        descriptor.type_name = "DummyService";
        descriptor.contract_id = descriptor.name + ".contract";
        descriptor.contract_version = 1;
        descriptor.placement.mode = mode;
        descriptor.placement.instances = instances;
        descriptor.endpoints = std::move(endpoints);

        return yuan::app::ServiceDefinition{
            std::move(descriptor),
            []() {
                return std::make_shared<DummyService>();
            }
        };
    }

    yuan::app::RuntimeWorkerConfig worker_config(std::size_t worker_count)
    {
        yuan::app::RuntimeWorkerConfig config;
        config.worker_count = worker_count;
        return config;
    }

    void test_all_workers_endpoint_requires_reuse_port()
    {
        std::vector<yuan::app::ServiceDefinition> definitions;
        definitions.push_back(make_definition(
            "http",
            yuan::app::PlacementMode::all_workers,
            {
                yuan::app::ServiceEndpoint{ "http", "127.0.0.1", 18080, "tcp" }
            }));

        const auto workers = yuan::app::build_worker_plan(worker_config(4), definitions);
        const auto endpoint_plan = yuan::app::EndpointManager::build_plan(workers);

        require(endpoint_plan.valid(), "all_workers endpoint plan should be valid");
        require(endpoint_plan.bindings.size() == 1, "replicated HTTP endpoint should group into one binding");
        require(endpoint_plan.bindings[0].strategy == yuan::app::EndpointBindingStrategy::reuse_port,
                "replicated HTTP endpoint should require reuse_port");
        require(endpoint_plan.bindings[0].owners.size() == 4,
                "replicated HTTP endpoint should have four owners");
        require(yuan::app::service_instance_requires_reuse_port(endpoint_plan, workers[0].service_instances[0]),
                "replicated service instance should request listener reuse_port");
    }

    void test_singleton_endpoint_is_private_bind()
    {
        std::vector<yuan::app::ServiceDefinition> definitions;
        definitions.push_back(make_definition(
            "admin",
            yuan::app::PlacementMode::singleton,
            {
                yuan::app::ServiceEndpoint{ "admin", "127.0.0.1", 18081, "tcp" }
            }));

        const auto workers = yuan::app::build_worker_plan(worker_config(4), definitions);
        const auto endpoint_plan = yuan::app::EndpointManager::build_plan(workers);

        require(endpoint_plan.valid(), "singleton endpoint plan should be valid");
        require(endpoint_plan.bindings.size() == 1, "singleton endpoint should produce one binding");
        require(endpoint_plan.bindings[0].strategy == yuan::app::EndpointBindingStrategy::private_bind,
                "singleton endpoint should use private bind");
        require(endpoint_plan.bindings[0].owners.size() == 1,
                "singleton endpoint should have one owner");
        require(!yuan::app::service_instance_requires_reuse_port(endpoint_plan, workers[0].service_instances[0]),
                "singleton service instance should not request listener reuse_port");
    }

    void test_different_services_same_endpoint_conflict()
    {
        std::vector<yuan::app::ServiceDefinition> definitions;
        definitions.push_back(make_definition(
            "http",
            yuan::app::PlacementMode::singleton,
            {
                yuan::app::ServiceEndpoint{ "http", "0.0.0.0", 18082, "tcp" }
            }));
        definitions.push_back(make_definition(
            "metrics",
            yuan::app::PlacementMode::singleton,
            {
                yuan::app::ServiceEndpoint{ "metrics", "0.0.0.0", 18082, "TCP" }
            }));

        const auto workers = yuan::app::build_worker_plan(worker_config(2), definitions);
        const auto endpoint_plan = yuan::app::EndpointManager::build_plan(workers);

        require(!endpoint_plan.valid(), "different logical services should conflict on same endpoint");
        require(endpoint_plan.bindings.size() == 1, "conflicting endpoint should group into one binding");
        require(endpoint_plan.bindings[0].strategy == yuan::app::EndpointBindingStrategy::conflict,
                "conflicting endpoint should be marked conflict");
        require(!endpoint_plan.diagnostics.empty(), "conflicting endpoint should report diagnostics");
    }

    void test_internal_endpoint_is_not_listening_binding()
    {
        std::vector<yuan::app::ServiceDefinition> definitions;
        definitions.push_back(make_definition(
            "internal-bus",
            yuan::app::PlacementMode::all_workers,
            {
                yuan::app::ServiceEndpoint{ "bus", "", 12345, "internal" }
            }));

        const auto workers = yuan::app::build_worker_plan(worker_config(3), definitions);
        const auto endpoint_plan = yuan::app::EndpointManager::build_plan(workers);

        require(endpoint_plan.valid(), "internal endpoints should not invalidate endpoint plan");
        require(endpoint_plan.bindings.empty(), "internal endpoints should not create listener bindings");
        require(!yuan::app::service_instance_requires_reuse_port(endpoint_plan, workers[0].service_instances[0]),
                "internal replicated endpoint should not request reuse_port");
    }

    void test_ephemeral_port_is_private_per_owner()
    {
        std::vector<yuan::app::ServiceDefinition> definitions;
        definitions.push_back(make_definition(
            "ephemeral",
            yuan::app::PlacementMode::all_workers,
            {
                yuan::app::ServiceEndpoint{ "ephemeral", "127.0.0.1", 0, "tcp" }
            }));

        const auto workers = yuan::app::build_worker_plan(worker_config(2), definitions);
        const auto endpoint_plan = yuan::app::EndpointManager::build_plan(workers);

        require(endpoint_plan.valid(), "ephemeral port endpoint plan should be valid");
        require(endpoint_plan.bindings.size() == 2, "ephemeral port should stay private per owner");
        for (const auto &binding : endpoint_plan.bindings) {
            require(binding.strategy == yuan::app::EndpointBindingStrategy::private_bind,
                    "ephemeral port binding should be private");
            require(binding.owners.size() == 1, "ephemeral port binding should have one owner");
        }
        require(!yuan::app::service_instance_requires_reuse_port(endpoint_plan, workers[0].service_instances[0]),
                "ephemeral replicated endpoint should not request reuse_port");
    }

    void test_sharded_same_worker_endpoint_uses_binding_plan()
    {
        std::vector<yuan::app::ServiceDefinition> definitions;
        definitions.push_back(make_definition(
            "mqtt",
            yuan::app::PlacementMode::sharded,
            {
                yuan::app::ServiceEndpoint{ "mqtt", "127.0.0.1", 1883, "tcp" }
            },
            2));

        const auto workers = yuan::app::build_worker_plan(worker_config(1), definitions);
        const auto endpoint_plan = yuan::app::EndpointManager::build_plan(workers);

        require(endpoint_plan.valid(), "same-service sharded endpoint plan should be valid");
        require(endpoint_plan.bindings.size() == 1,
                "same-worker sharded service instances should share one binding");
        require(endpoint_plan.bindings[0].strategy == yuan::app::EndpointBindingStrategy::reuse_port,
                "same-worker sharded fixed endpoint should require reuse_port");
        require(endpoint_plan.bindings[0].owners.size() == 2,
                "same-worker sharded fixed endpoint should have two binding owners");
        require(yuan::app::service_instance_requires_reuse_port(endpoint_plan, workers[0].service_instances[0]),
                "first sharded service instance should use endpoint-plan reuse_port");
        require(yuan::app::service_instance_requires_reuse_port(endpoint_plan, workers[0].service_instances[1]),
                "second sharded service instance should use endpoint-plan reuse_port");
    }

    void test_bootstrap_rejects_conflicting_endpoint_plan()
    {
        yuan::app::RuntimeContext context;
        context.app_name = "endpoint-conflict-bootstrap-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 2;
        context.runtime_workers.worker_count = 2;

        yuan::app::Application app(context);

        auto left = make_definition(
            "left",
            yuan::app::PlacementMode::singleton,
            {
                yuan::app::ServiceEndpoint{ "left", "127.0.0.1", 18083, "tcp" }
            });
        auto right = make_definition(
            "right",
            yuan::app::PlacementMode::singleton,
            {
                yuan::app::ServiceEndpoint{ "right", "127.0.0.1", 18083, "tcp" }
            });

        require(app.add_service(left.descriptor, left.factory),
                "left conflicting service should register");
        require(app.add_service(right.descriptor, right.factory),
                "right conflicting service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(!bootstrap.run(), "bootstrap should reject conflicting endpoint plan before starting workers");
    }
}

int main()
{
    test_all_workers_endpoint_requires_reuse_port();
    test_singleton_endpoint_is_private_bind();
    test_different_services_same_endpoint_conflict();
    test_internal_endpoint_is_not_listening_binding();
    test_ephemeral_port_is_private_per_owner();
    test_sharded_same_worker_endpoint_uses_binding_plan();
    test_bootstrap_rejects_conflicting_endpoint_plan();
    std::cout << "endpoint manager tests passed\n";
    return 0;
}
