#include "common/gm_command_registry.h"

#include <utility>

namespace yuan::game::server
{
    GmCommandRegistry &GmCommandRegistry::instance()
    {
        static GmCommandRegistry registry;
        return registry;
    }

    void GmCommandRegistry::register_command(GmCommandDefinition definition)
    {
        if (definition.command.empty()) {
            return;
        }
        definitions_[definition.command] = std::move(definition);
    }

    std::optional<GmCommandDefinition> GmCommandRegistry::find(const std::string &command) const
    {
        const auto it = definitions_.find(command);
        if (it == definitions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void register_builtin_gm_commands()
    {
        static bool registered = false;
        if (registered) {
            return;
        }
        registered = true;

        GmCommandRegistry::instance().register_command(GmCommandDefinition{
            "set_time_offset_seconds",
            GameServiceType::global});
    }

    std::optional<yuan::rpc::Route> gm_execute_route_for(GameServiceType executor_type)
    {
        switch (executor_type) {
            case GameServiceType::global:
                return game_route::global_gm_execute();
            default:
                return std::nullopt;
        }
    }
}
