#include "net/ftp/server/commands/pasv.h"
#include "net/ftp/common/def.h"
#include "net/ftp/common/response_code.h"
#include "net/ftp/common/session.h"
#include "net/base/socket/inet_address.h"

namespace net::ftp 
{
    REGISTER_COMMAND_IMPL(CommandPasv);

    FtpCommandResponse CommandPasv::execute(FtpSession *session, const std::string &args)
    {
        session->start_file_stream({"", 12124}, StreamMode::Receiver);
        return {FtpResponseCode::__227__, "192.168.96.1 12124"};
    }

    CommandType CommandPasv::get_command_type()
    {
        return CommandType::cmd_pasv;
    }

    std::string CommandPasv::get_comand_name()
    {
        return "PASV";
    }
}