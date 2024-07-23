#include "net/ftp/server/server_session.h"
#include "net/ftp/common/def.h"
#include "net/ftp/common/response_code.h"
#include "net/ftp/server/command.h"

#include <iostream>
#include <string>

namespace net::ftp 
{
    ServerFtpSession::ServerFtpSession(Connection *conn, FtpApp *app, bool keepUtilSent) : FtpSession(conn, app, WorkMode::server, keepUtilSent)
    {
        
    }

    ServerFtpSession::~ServerFtpSession()
    {

    }
    
    void ServerFtpSession::on_read(Connection *conn)
    {
        command_parser_.set_buff(conn->get_input_buff(true));
        const auto &cmds = command_parser_.split_cmds(delimiter.begin(), " ");
        if (cmds.empty()) {
            std::cout << "command did not receive all!\n";
            return;
        }

        for (const auto &item : cmds) {
            const std::string &cmd = item.cmd_;
            std::cout << "cmd >>> " << cmd << '\n';
            auto command = CommandFactory::get_instance()->find_command(cmd);
            if (!command) {
                std::cout << "not support command: " << cmd << '\n';
                on_error(0);
                return;
            }

            std::cout << "call commanmd: " << cmd << ", args: " << item.args_ << '\n';
            const auto &res = command->execute(this, item.args_);
            if (res.code_ == FtpResponseCode::invalid) {
                return;
            }

            conn->get_output_buff()->write_string(std::to_string((int)res.code_));
            conn->get_output_buff()->write_string(" ");
            conn->get_output_buff()->write_string(res.body_);
            conn->get_output_buff()->write_string("\r\n");
            conn->send();

            if (res.close_) {
                quit();
                break;
            }
        }
    }
}