#ifndef YUAN_GAME_SERVER_COMMON_GM_COMMAND_REGISTRY_H
#define YUAN_GAME_SERVER_COMMON_GM_COMMAND_REGISTRY_H

#include "common/game_messages.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace yuan::game::server
{
    struct GmCommandDefinition
    {
        std::string command;
        GameServiceType executor_type = GameServiceType::world;
    };

    class GmCommandRegistry
    {
    public:
        static GmCommandRegistry &instance();

        void register_command(GmCommandDefinition definition);
        [[nodiscard]] std::optional<GmCommandDefinition> find(const std::string &command) const;

    private:
        std::unordered_map<std::string, GmCommandDefinition> definitions_;
    };

    void register_builtin_gm_commands();
    [[nodiscard]] std::optional<yuan::rpc::Route> gm_execute_route_for(GameServiceType executor_type);
}

#endif
