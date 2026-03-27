#include "server/command.h"

namespace yuan::net::ftp
{
    CommandFactory::~CommandFactory()
    {
        for (auto &item : commands) {
            if (item.second) {
                delete item.second;
            }
        }
    }

    Command *CommandFactory::find_command(const std::string &cmd)
    {
        ensure_all_commands_registered();
        auto it = commands.find(cmd);
        return it == commands.end() ? nullptr : it->second;
    }

    bool CommandFactory::register_command(Command *cmdImpl)
    {
        if (cmdImpl) {
            if (commands.find(cmdImpl->get_comand_name()) != commands.end()) {
                delete cmdImpl;
                return false;
            }
            commands[cmdImpl->get_comand_name()] = cmdImpl;
            return true;
        }
        return false;
    }
}
