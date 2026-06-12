#ifndef YUAN_GAME_BASE_GAME_SERVER_H
#define YUAN_GAME_BASE_GAME_SERVER_H

#include "game_base/command_router.h"
#include "game_base/algorithm/algorithms.h"
#include "game_base/entity_mailbox.h"
#include "game_base/id_generator.h"
#include "game_base/room.h"
#include "game_base/service.h"
#include "game_base/session_registry.h"
#include "game_base/types.h"
#include "game_base/world_partition.h"
#include "yuan/rpc/rpc.h"

#include <chrono>
#include <utility>

namespace yuan::game_base
{
    struct GameServerOptions
    {
        GameGenre genre = GameGenre::generic;
        NodeDescriptor node;
        Milliseconds tick_interval{50};
    };

    class GameServerKernel
    {
    public:
        explicit GameServerKernel(GameServerOptions options = {})
            : options_(std::move(options)), snowflake_(options_.node.key.id)
        {
        }

        [[nodiscard]] const GameServerOptions &options() const
        {
            return options_;
        }

        [[nodiscard]] ServiceHost &services()
        {
            return services_;
        }

        [[nodiscard]] SessionRegistry &sessions()
        {
            return sessions_;
        }

        [[nodiscard]] CommandRouter &commands()
        {
            return commands_;
        }

        [[nodiscard]] EntityMailbox &mailbox()
        {
            return mailbox_;
        }

        [[nodiscard]] WorldPartition &world()
        {
            return world_;
        }

        [[nodiscard]] RoomRegistry &rooms()
        {
            return rooms_;
        }

        [[nodiscard]] yuan::rpc::Server &rpc_server()
        {
            return rpc_server_;
        }

        std::uint64_t next_local_id()
        {
            return ids_.next();
        }

        std::uint64_t next_global_id(std::chrono::milliseconds unix_ms)
        {
            return snowflake_.next(unix_ms);
        }

        bool start()
        {
            return services_.start_all();
        }

        void stop()
        {
            services_.stop_all();
        }

        void tick(Milliseconds delta)
        {
            services_.tick_all(delta);
        }

    private:
        GameServerOptions options_;
        IdGenerator ids_;
        SnowflakeIdGenerator snowflake_;
        ServiceHost services_;
        SessionRegistry sessions_;
        CommandRouter commands_;
        EntityMailbox mailbox_;
        WorldPartition world_;
        RoomRegistry rooms_;
        yuan::rpc::Server rpc_server_;
    };
}

#endif
