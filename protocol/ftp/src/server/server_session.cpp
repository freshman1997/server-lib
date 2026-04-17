#include "server/server_session.h"
#include "common/def.h"
#include "server/command.h"

#include "logger.h"

namespace yuan::net::ftp
{
    ServerFtpSession::ServerFtpSession(Connection * conn, FtpApp * app, bool keepUtilSent, bool async_mode)
        : FtpSession(conn, app, WorkMode::server, keepUtilSent, async_mode)
    {
    }
    ServerFtpSession::~ServerFtpSession()
    {
    }

    void ServerFtpSession::on_read(Connection * conn)
    {
        auto buff = conn->take_input_byte_buffer();
        command_parser_.set_buff(buff);
        const auto &cmds = command_parser_.split_cmds(delimiter, " ");
        for (const auto &item : cmds) {
            LOG_DEBUG("ftp server cmd={} args={}", item.cmd_, item.args_);
            auto command = CommandFactory::get_instance()->find_command(item.cmd_);
            if (!command) {
                conn->append_output("500 Unsupported command.\r\n");
                conn->flush();
                continue;
            }
            const auto &res = command->execute(this, item.args_);
            LOG_DEBUG("ftp server response code={} body={}", static_cast<int>(res.code_), res.body_);
            if (res.code_ == FtpResponseCode::invalid) {
                continue;
            }
            conn->append_output(std::to_string((int)res.code_));
            conn->append_output(" ");
            conn->append_output(res.body_);
            conn->append_output("\r\n");
            conn->flush();
            if (res.close_) {
                quit();
                break;
            }
        }
    }
}
