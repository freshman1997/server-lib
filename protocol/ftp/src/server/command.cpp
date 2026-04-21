#include "server/command.h"

namespace yuan::net::ftp
{
    CommandFactory::~CommandFactory()
    {
    }

    Command *CommandFactory::find_command(const std::string &cmd)
    {
        ensure_all_commands_registered();
        auto it = commands.find(cmd);
        return it == commands.end() ? nullptr : (it->second ? &*it->second : nullptr);
    }

    bool CommandFactory::register_command(Command *cmdImpl)
    {
        if (cmdImpl) {
            if (commands.find(cmdImpl->get_command_name()) != commands.end()) {
                return false;
            }
            commands[cmdImpl->get_command_name()] = std::unique_ptr<Command>(cmdImpl);
            return true;
        }
        return false;
    }
}
