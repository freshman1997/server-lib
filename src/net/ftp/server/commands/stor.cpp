#include "net/ftp/server/commands/stor.h"
#include "net/ftp/common/def.h"
#include "net/ftp/common/response_code.h"
#include "net/ftp/common/session.h"

#include <iostream>

namespace net::ftp 
{
    REGISTER_COMMAND_IMPL(CommandStor);

    FtpCommandResponse CommandStor::execute(FtpSession *session, const std::string &args)
    {
        std::cout << "store file: D:/misc/" << args << '\n';
        session->get_file_manager()->set_work_filepath("D:/misc/" + args);
        auto file = session->get_file_manager()->get_next_file();
        file->dest_name_ = file->origin_name_;
        file->ready_ = true;
        file->file_size_ = 393978331;
        session->set_work_file(file);
        return {FtpResponseCode::__200__, "start receiving file from client!!!"};
    }

    CommandType CommandStor::get_command_type()
    {
        return CommandType::cmd_stor;
    }

    std::string CommandStor::get_comand_name()
    {
        return "STOR";
    }
}