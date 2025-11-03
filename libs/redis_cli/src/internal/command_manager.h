#ifndef __YUAN_REDIS_COMMAND_MANAGER_H__
#define __YUAN_REDIS_COMMAND_MANAGER_H__
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>

#include "../command.h"
#include "singleton/singleton.h"

namespace yuan::redis
{
    class CommandManager : public singleton::Singleton<CommandManager>
    {
    public:
        CommandManager();
    
        std::shared_ptr<Command> get_command(const std::string &name);

        bool add_command(const std::string &name, std::shared_ptr<Command> command);

    private:
        std::unordered_map<std::string, std::queue<std::shared_ptr<Command>>> command_queue_map_;
    };
}

#endif // __YUAN_REDIS_COMMAND_MANAGER_H__
