#include "server/commands/list.h"
#include "common/response_code.h"

namespace yuan::net::ftp 
{
    REGISTER_COMMAND_IMPL(CommandList);

    FtpCommandResponse CommandList::execute(FtpSession *session, const std::string &args)
    {
        return {FtpResponseCode::__200__, "no file in this dir"};
    }

    CommandType CommandList::get_command_type()
    {
        return CommandType::cmd_list;
    }

    std::string CommandList::get_comand_name()
    {
        return "LIST";
    }
}