#include "server/server_session.h"
#include "common/def.h"
#include "server/command.h"

#include <iostream>

namespace yuan::net::ftp
{
    ServerFtpSession::ServerFtpSession(Connection *conn, FtpApp *app, bool keepUtilSent) : FtpSession(conn, app, WorkMode::server, keepUtilSent) {}
    ServerFtpSession::~ServerFtpSession() {}

    void ServerFtpSession::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff(true);
        command_parser_.set_buff(buff);
        const auto &cmds = command_parser_.split_cmds(delimiter, " ");
        for (const auto &item : cmds) {
            std::cout << "server command cmd=" << item.cmd_ << " args=" << item.args_ << "\n";
            auto command = CommandFactory::get_instance()->find_command(item.cmd_);
            auto outBuff = conn->get_output_linked_buffer()->get_current_buffer();
            if (!command) {
                outBuff->write_string("500 Unsupported command.\r\n");
                conn->flush();
                continue;
            }
            const auto &res = command->execute(this, item.args_);
            std::cout << "server response code=" << static_cast<int>(res.code_) << " body=" << res.body_ << "\n";
            if (res.code_ == FtpResponseCode::invalid) {
                continue;
            }
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
