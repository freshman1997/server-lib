#ifndef YUAN_GAME_BASE_COMMAND_ROUTER_H
#define YUAN_GAME_BASE_COMMAND_ROUTER_H

#include "game_base/types.h"

#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace yuan::game_base
{
    struct CommandContext
    {
        SessionId session = 0;
        PlayerId player = 0;
        NodeId source_node = 0;
        Tags metadata;
    };

    struct Command
    {
        CommandId id = 0;
        Bytes payload;
        DeliveryGuarantee delivery = DeliveryGuarantee::best_effort;
    };

    struct CommandResult
    {
        bool ok = true;
        std::string error;
        Bytes payload;
    };

    using CommandHandler = std::function<CommandResult(const CommandContext &, const Command &)>;

    class CommandRouter
    {
    public:
        bool bind(CommandId id, CommandHandler handler)
        {
            if (id == 0 || !handler) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            return handlers_.emplace(id, std::move(handler)).second;
        }

        bool unbind(CommandId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return handlers_.erase(id) != 0;
        }

        CommandResult dispatch(const CommandContext &context, const Command &command) const
        {
            CommandHandler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = handlers_.find(command.id);
                if (it != handlers_.end()) {
                    handler = it->second;
                }
            }

            if (!handler) {
                return {false, "command handler not found", {}};
            }
            return handler(context, command);
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<CommandId, CommandHandler> handlers_;
    };
}

#endif
