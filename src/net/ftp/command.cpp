#include "net/ftp/command.h"

namespace net::ftp 
{
    Command * CommandFactory::find_command(const std::string &cmd)
    {
        auto it = commands.find(cmd);
        return it == commands.end() ? nullptr : it->second;
    }

    bool CommandFactory::register_command(const std::string &cmd, Command *cmdImpl)
    {
        if (cmdImpl) {
            commands[cmd] = cmdImpl;
            return true;
        }
        return false;
    }
}