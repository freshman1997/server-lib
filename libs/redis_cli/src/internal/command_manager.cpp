#include "command_manager.h"

namespace yuan::redis
{
    CommandManager::CommandManager() = default;

    std::shared_ptr<Command> CommandManager::get_command(const std::string &name)
    {
        auto it = command_queue_map_.find(name);
        if (it != command_queue_map_.end())
        {
            if (it->second.empty())
            {
                return nullptr;
            }
            
            auto cmd = it->second.front();
            it->second.pop();
            return cmd;
        } else {
            return nullptr;
        }
    }

    bool CommandManager::add_command(const std::string &name, std::shared_ptr<Command> command)
    {
        if (!command)
        {
            return false;
        }

        command_queue_map_[name].push(command);

        return true;
    }
}