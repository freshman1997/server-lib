#include "server/commands/allo.h"
#include "common/response_code.h"
#include "common/session.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandAllo::execute(FtpSession *session, const std::string &args)
    {
        if (session && !args.empty()) {
            try {
                session->set_item_value<int32_t>("upload_file_size", std::stoi(args));
            } catch (...) {
                return {FtpResponseCode::__501__, "Allocation size is invalid."};
            }
        }
        return {FtpResponseCode::__200__, "ALLO command ignored."};
    }

    CommandType CommandAllo::get_command_type() { return CommandType::cmd_allo; }
    std::string CommandAllo::get_command_name() { return "ALLO"; }
}
#include "common/session.h"
