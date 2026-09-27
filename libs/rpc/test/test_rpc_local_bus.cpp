#include "yuan/rpc/local_bus.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    yuan::rpc::Response response_for(const yuan::rpc::Message &message, yuan::rpc::RpcStatus status = yuan::rpc::RpcStatus::ok)
    {
        yuan::rpc::Response response;
        response.request_id = message.request_id;
        response.set_continuation_id(message.continuation_id());
        response.status = status;
        response.payload = message.payload;
        return response;
    }

    int test_numeric_route()
    {
        yuan::rpc::LocalBus bus;
        yuan::rpc::Route route{12, 34, {}};
        if (!require(bus.bind(route, [](const yuan::rpc::Message &message) { return response_for(message); }),
                     "numeric route should bind")) {
            return 10;
        }
        if (!require(!bus.bind(route, [](const yuan::rpc::Message &message) { return response_for(message); }),
                     "duplicate numeric route should not bind")) {
            return 11;
        }

        yuan::rpc::Message message;
        message.request_id = 77;
        message.set_continuation_id(88);
        message.route = route;
        message.payload = {1, 2, 3};
        const auto response = bus.dispatch(message);
        if (!require(response.status == yuan::rpc::RpcStatus::ok && response.request_id == 77 &&
                         response.continuation_id() == 88 && response.payload == message.payload,
                     "numeric route dispatch mismatch")) {
            return 12;
        }
        return 0;
    }

    int test_named_route_and_unbind()
    {
        yuan::rpc::LocalBus bus;
        yuan::rpc::Route route;
        route.name = "service.echo";
        if (!require(bus.bind(route, [](const yuan::rpc::Message &message) { return response_for(message); }),
                     "named route should bind")) {
            return 20;
        }
        if (!require(bus.size() == 1, "bus size should include named route")) {
            return 21;
        }

        yuan::rpc::Message message;
        message.route = route;
        const auto response = bus.dispatch(message);
        if (!require(response.status == yuan::rpc::RpcStatus::ok, "named route dispatch should succeed")) {
            return 22;
        }
        if (!require(bus.unbind(route) && bus.size() == 0, "named route should unbind")) {
            return 23;
        }

        const auto missing = bus.dispatch(message);
        if (!require(missing.status == yuan::rpc::RpcStatus::not_found, "unbound route should be missing")) {
            return 24;
        }
        return 0;
    }

    int test_handler_exception()
    {
        yuan::rpc::LocalBus bus;
        yuan::rpc::Route route{56, 78, {}};
        if (!require(bus.bind(route, [](const yuan::rpc::Message &) -> yuan::rpc::Response {
                         throw std::runtime_error("handler failure");
                     }),
                     "exception route should bind")) {
            return 30;
        }

        yuan::rpc::Message message;
        message.route = route;
        const auto response = bus.dispatch(message);
        if (!require(response.status == yuan::rpc::RpcStatus::internal_error && response.error == "handler failure",
                     "handler exception should become internal error")) {
            return 31;
        }
        return 0;
    }

    int test_high_frequency_dispatch()
    {
        yuan::rpc::LocalBus bus;
        yuan::rpc::Route route{90, 91, {}};
        std::size_t invocation_count = 0;
        if (!require(bus.bind(route, [&invocation_count](const yuan::rpc::Message &message) {
                         ++invocation_count;
                         return response_for(message);
                     }),
                     "high frequency route should bind")) {
            return 40;
        }

        yuan::rpc::Message message;
        message.route = route;
        for (std::size_t i = 0; i < 10000; ++i) {
            message.request_id = i + 1;
            const auto response = bus.dispatch(message);
            if (response.status != yuan::rpc::RpcStatus::ok || response.request_id != i + 1) {
                return 41;
            }
        }
        if (!require(invocation_count == 10000, "high frequency dispatch count mismatch")) {
            return 42;
        }
        return 0;
    }
}

int main()
{
    if (const int rc = test_numeric_route(); rc != 0) {
        return rc;
    }
    if (const int rc = test_named_route_and_unbind(); rc != 0) {
        return rc;
    }
    if (const int rc = test_handler_exception(); rc != 0) {
        return rc;
    }
    if (const int rc = test_high_frequency_dispatch(); rc != 0) {
        return rc;
    }
    return EXIT_SUCCESS;
}
