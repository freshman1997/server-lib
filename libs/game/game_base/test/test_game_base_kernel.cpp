#include "game_base/game_server.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

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

    class CounterService final : public yuan::game_base::ServiceBase
    {
    public:
        CounterService()
            : ServiceBase("counter")
        {
        }

        int ticks = 0;

    private:
        void on_tick(yuan::game_base::Milliseconds) override
        {
            ++ticks;
        }
    };
}

int main()
{
    using namespace yuan::game_base;

    GameServerOptions options;
    options.genre = GameGenre::mmorpg;
    options.node.key.id = 7;
    options.node.key.role = ServerRole::world;
    options.node.key.shard = 2;

    GameServerKernel kernel(options);
    auto service = std::make_shared<CounterService>();
    if (!require(kernel.services().add(service), "service add should succeed")) {
        return 1;
    }
    if (!require(kernel.start(), "kernel start should succeed")) {
        return 2;
    }
    kernel.tick(std::chrono::milliseconds(50));
    if (!require(service->ticks == 1, "service should tick once")) {
        return 3;
    }

    Session session;
    session.id = 11;
    session.player = 1001;
    session.gateway = 3;
    if (!require(kernel.sessions().bind(session), "session bind should succeed")) {
        return 4;
    }
    if (!require(kernel.sessions().find_by_player(1001).has_value(), "player session lookup should succeed")) {
        return 5;
    }

    if (!require(kernel.commands().bind(101, [](const CommandContext &context, const Command &command) {
            CommandResult result;
            result.ok = context.player == 1001 && command.id == 101;
            result.payload = command.payload;
            return result;
        }), "command bind should succeed")) {
        return 6;
    }
    CommandContext context;
    context.player = 1001;
    Command command;
    command.id = 101;
    command.payload = {1, 2, 3};
    const auto command_result = kernel.commands().dispatch(context, command);
    if (!require(command_result.ok && command_result.payload.size() == 3, "command dispatch should succeed")) {
        return 7;
    }

    EntityLocation location;
    location.entity = 42;
    location.scene = 9;
    location.position = {33.0F, 0.0F, 63.0F};
    if (!require(kernel.world().upsert(location), "world location upsert should succeed")) {
        return 8;
    }
    const auto cell = kernel.world().cell_for(9, location.position);
    if (!require(kernel.world().query_cell(cell).size() == 1, "world cell query should return entity")) {
        return 9;
    }

    Room room;
    room.id = 501;
    room.mode = "moba-5v5";
    if (!require(kernel.rooms().create(room), "room create should succeed")) {
        return 10;
    }
    if (!require(kernel.rooms().join(501, 1001), "room join should succeed")) {
        return 11;
    }

    yuan::rpc::Route route;
    route.name = "game.echo";
    if (!require(kernel.rpc_server().register_handler(route, [](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.status = yuan::rpc::RpcStatus::ok;
            response.payload = message.payload;
            return response;
        }), "rpc handler register should succeed")) {
        return 12;
    }

    yuan::rpc::InProcessChannel channel(kernel.rpc_server());
    yuan::rpc::Client client(channel);
    bool rpc_done = false;
    bool rpc_ok = false;
    if (!require(client.call(route, {4, 5, 6}, [&](yuan::rpc::Response response) {
            rpc_done = true;
            rpc_ok = response.status == yuan::rpc::RpcStatus::ok && response.payload.size() == 3;
        }), "rpc call should be sent")) {
        return 13;
    }
    if (!require(rpc_done && rpc_ok, "in-process rpc should complete")) {
        return 14;
    }

    const auto global_id = kernel.next_global_id(std::chrono::milliseconds(1704067200001ULL));
    if (!require(global_id != 0, "global id should not be zero")) {
        return 15;
    }

    kernel.stop();
    return EXIT_SUCCESS;
}
