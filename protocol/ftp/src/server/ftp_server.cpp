#include "server/ftp_server.h"
#include "coroutine/io_result.h"
#include "coroutine/stream_io_awaitable.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/channel/channel.h"
#include "server/server_session.h"
#include "server/command.h"
#include "server/context.h"
#include "common/def.h"

#include "logger.h"
#include <memory>

namespace yuan::net::ftp
{
    namespace
    {
        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

    FtpServer::FtpServer()
    {
    }

    FtpServer::~FtpServer()
    {
        LOG_INFO("ftp server exiting");
        quit();
    }

    bool FtpServer::serve(int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return serve(port, *owned_runtime_);
    }

    bool FtpServer::serve(int port, NetworkRuntime & runtime)
    {
        if (!listener_.bind(port, runtime)) {
            LOG_ERROR("cant listen on port: {}!", port);
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        listener_.set_connection_handler(
            [this](net::AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        if (owned_runtime_) {
            auto accept_task = listener_.run_async();
            accept_task.resume();
            owned_runtime_->run();
            listener_.close();
            owned_runtime_.reset();
        }

        return true;
    }

    coroutine::Task<void> FtpServer::handle_connection(net::AsyncConnectionContext ctx)
    {
        auto conn = ctx.connection();
        if (!conn) {
            co_return;
        }

        auto session = std::make_unique<ServerFtpSession>(conn, this, false, true);
        active_sessions_.insert(ptr_of(session));

        ctx.append_output("220 Welcome to FTP server.\r\n");
        ctx.flush();

        while (ctx.is_connected()) {
            auto read_result = co_await ctx.read_async();
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            auto &parser = session->command_parser();
            parser.set_buff(std::move(read_result.data));
            const auto &cmds = parser.split_cmds(delimiter, " ");

            for (const auto &item : cmds) {
                LOG_DEBUG("ftp server cmd={} args={}", item.cmd_, item.args_);
                auto command = CommandFactory::get_instance()->find_command(item.cmd_);
                if (!command) {
                    ctx.append_output("500 Unsupported command.\r\n");
                    ctx.flush();
                    continue;
                }

                const auto &res = command->execute(ptr_of(session), item.args_);
                LOG_DEBUG("ftp server response code={} body={}", static_cast<int>(res.code_), res.body_);
                if (res.code_ == FtpResponseCode::invalid) {
                    continue;
                }

                ctx.append_output(std::to_string((int)res.code_));
                ctx.append_output(" ");
                ctx.append_output(res.body_);
                ctx.append_output("\r\n");
                ctx.flush();

                if (session->has_pending_transfer()) {
                    auto data_listener = session->take_data_listener();
                    auto *file_info = session->take_pending_file_info();
                    session->clear_pending_transfer();

                    if (data_listener && file_info) {
                        co_await data_transfer(ctx.runtime_view(), std::move(data_listener), file_info, ctx);
                    }
                }

                if (res.close_) {
                    goto done;
                }
            }
        }

    done:
        return_passive_port(ptr_of(session));
        on_session_closed(ptr_of(session));
        session->detach_async();
        co_return;
    }

    coroutine::Task<void> FtpServer::data_transfer(
        coroutine::RuntimeView rv,
        std::unique_ptr<net::AsyncListenerHost> listener,
        FtpFileInfo * file_info,
        net::AsyncConnectionContext & control_ctx)
    {
        auto data_conn = co_await listener->accept_async();
        if (!data_conn) {
            if (control_ctx.is_connected()) {
                control_ctx.append_output("425 Can't open data connection.\r\n");
                control_ctx.flush();
            }
            co_return;
        }

        data_conn->set_connection_handler(std::shared_ptr<net::ConnectionHandler>{});
        rv.register_connection(data_conn, nullptr);

        net::AsyncConnectionContext data_ctx(data_conn, rv);

        if (auto stream = std::dynamic_pointer_cast<StreamTransport>(data_conn)) {
            if (auto *channel = stream->stream_channel()) {
                if (file_info->mode_ == StreamMode::Receiver) {
                    channel->disable_write();
                    channel->enable_read();
                } else {
                    channel->disable_read();
                    channel->enable_write();
                }
                rv.update_channel(channel);
            }
        }

        bool transfer_ok = false;

        if (file_info->mode_ == StreamMode::Sender) {
            while (!file_info->is_completed()) {
                ::yuan::buffer::ByteBuffer buff(default_write_buff_size);
                int ret = file_info->read_file(default_write_buff_size, buff);
                if (ret < 0) {
                    break;
                }
                if (buff.readable_bytes() > 0) {
                    data_ctx.write_and_flush(buff);
                    auto flush_result = co_await data_ctx.flush_async();
                    if (flush_result.status != coroutine::IoStatus::success) {
                        break;
                    }
                }
                if (file_info->is_completed()) {
                    transfer_ok = true;
                    break;
                }
            }
        } else {
            while (true) {
                auto read_result = co_await data_ctx.read_async();
                if (read_result.status != coroutine::IoStatus::success) {
                    break;
                }
                int ret = file_info->write_file(read_result.data);
                if (ret < 0) {
                    break;
                }
                if (file_info->is_completed()) {
                    transfer_ok = true;
                    break;
                }
            }
            if (file_info->file_size_ == 0 && !file_info->is_completed()) {
                file_info->state_ = FileState::processed;
                transfer_ok = true;
            }
        }

        co_await data_ctx.close_async();

        if (control_ctx.is_connected()) {
            if (transfer_ok) {
                control_ctx.append_output("226 Transfer complete.\r\n");
            } else {
                control_ctx.append_output("426 Connection closed; transfer aborted.\r\n");
            }
            control_ctx.flush();
        }

        co_return;
    }

    bool FtpServer::is_ok()
    {
        return listener_.is_listening();
    }

    NetworkRuntime *FtpServer::get_runtime()
    {
        return listener_.runtime();
    }

    void FtpServer::on_session_closed(FtpSession * session)
    {
        if (!session) {
            return;
        }
        active_sessions_.erase(session);
        LOG_INFO("ftp session closed, remaining sessions: {}", active_sessions_.size());
    }

    void FtpServer::return_passive_port(FtpSession * session)
    {
        auto passive_addr = session->get_passive_addr();
        if (passive_addr.has_value()) {
            ServerContext::get_instance()->remove_stream_port(passive_addr->get_port());
            session->clear_passive_addr();
        }
    }

    void FtpServer::quit()
    {
        if (closing_) {
            return;
        }

        closing_ = true;

        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }
}
