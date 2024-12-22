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

    Command * CommandFactory::find_command(const std::string &cmd)
    {
        auto it = commands.find(cmd);
        return it == commands.end() ? nullptr : it->second;
    }

    bool CommandFactory::register_command(Command *cmdImpl)
    {
        if (cmdImpl) {
            commands[cmdImpl->get_comand_name()] = cmdImpl;
            return true;
        }
        return false;
    }
}