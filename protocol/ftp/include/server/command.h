#ifndef __NET_FTP_SERVER_SERVER_COMMAND_H__
#define __NET_FTP_SERVER_SERVER_COMMAND_H__
#include <memory>
#include <string>
#include <unordered_map>

#include "common/def.h"
#include "singleton/singleton.h"

namespace yuan::net::ftp
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
        cmd_list,
        cmd_user,
        cmd_stor,
        cmd_retr,
        cmd_pasv,
        cmd_mkd,
        cmd_rmd,
        cmd_rnfr,
        cmd_rnto,
        cmd_port,
        cmd_stat,
        cmd_stou,
        cmd_rein,
        cmd_site,
    };

    class Command
    {
    public:
        virtual ~Command() {}
        virtual FtpCommandResponse execute(FtpSession *session, const std::string &args) = 0;
        virtual CommandType get_command_type() = 0;
        virtual std::string get_comand_name() = 0;
    };

    class CommandFactory : public singleton::Singleton<CommandFactory>, public std::enable_shared_from_this<CommandFactory>
    {
    public:
        ~CommandFactory();
        Command * find_command(const std::string &cmd);
        bool register_command(Command *cmdImpl);

    private:
        std::unordered_map<std::string, Command *> commands;
    };

    void ensure_all_commands_registered();
    extern bool _0_reg_command_0_;

#define REGISTER_COMMAND_IMPL(type, ...) \
    namespace \
    { \
        bool _0_reg_command_0_ = CommandFactory::get_instance()->register_command(new type(__VA_ARGS__)); \
    }
}

#endif
