#include "server/server_session.h"
#include "common/def.h"
#include "server/command.h"

#include <iostream>

namespace yuan::net::ftp 
{
    ServerFtpSession::ServerFtpSession(Connection *conn, FtpApp *app, bool keepUtilSent) : FtpSession(conn, app, WorkMode::server, keepUtilSent)
    {
        
    }

    ServerFtpSession::~ServerFtpSession()
    {

    }
    
    void ServerFtpSession::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff(true);
        command_parser_.set_buff(buff);
        const auto &cmds = command_parser_.split_cmds(delimiter.data(), " ");
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

            auto outBuff = conn->get_output_linked_buffer()->get_current_buffer();
            outBuff->write_string(std::to_string((int)res.code_));
            outBuff->write_string(" ");
            outBuff->write_string(res.body_);
            outBuff->write_string("\r\n");
            conn->flush();

            if (res.close_) {
                quit();
                break;
            }
        }
    }
}