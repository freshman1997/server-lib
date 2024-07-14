#ifndef __NET_FTP_COMMAND_H__
#define __NET_FTP_COMMAND_H__
#include "singleton/singleton.h"
#include <string>
#include <unordered_map>

namespace net::ftp 
{
    class FtpSession;

    enum class CommandType : short
    {
        cmd_acct,
        cmd_abort,
        cmd_allo,
        cmd_appe,
        cmd_cdup,
        cmd_dele,
        cmd_cwd,
        cmd_help,
        cmd_list
    };

    class Command
    {
    public:
        virtual int execute(FtpSession *session, std::string &args) = 0;

        virtual CommandType get_command_type() = 0;

        virtual std::string get_comand_name() = 0;        
    };

    class CommandFactory
    {
    public:
        Command * find_command(const std::string &cmd);

        bool register_command(const std::string &cmd, Command *cmdImpl);

    private:
        std::unordered_map<std::string, Command *> commands;
    };

    extern bool _0_reg_command_0_;

    #define REGISTER_COMMAND_IMPL(name, type, ...) \
        bool _0_reg_command_0_ = singleton::Singleton<CommandFactory>().register_command(name, new type(...));
}

#endif