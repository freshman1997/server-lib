#include "command_manager.h"

namespace yuan::redis
{
    CommandManager::CommandManager() = default;

    std::shared_ptr<Command> CommandManager::get_command()
    {
        if (command_queue_.empty())
        {
            return nullptr;
        }

        auto cmd = command_queue_.front();
        command_queue_.pop();
        return cmd;
    }

    bool CommandManager::add_command(std::shared_ptr<Command> command)
    {
        if (!command)
        {
            return false;
        }

        command_queue_.push(command);
        return true;
    }
}