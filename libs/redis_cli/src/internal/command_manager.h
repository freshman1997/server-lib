#ifndef __YUAN_REDIS_COMMAND_MANAGER_H__
#define __YUAN_REDIS_COMMAND_MANAGER_H__
#include <memory>
#include <queue>

#include "../command.h"
#include "singleton/singleton.h"

namespace yuan::redis
{
    class CommandManager : public singleton::Singleton<CommandManager>
    {
    public:
        CommandManager();
    
        std::shared_ptr<Command> get_command();

        bool add_command(std::shared_ptr<Command> command);

    private:
        std::queue<std::shared_ptr<Command>> command_queue_;
    };
}

#endif // __YUAN_REDIS_COMMAND_MANAGER_H__
