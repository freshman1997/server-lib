#include "server/ftp_server.h"
#include "coroutine/io_result.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/connect_awaitable.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/channel/channel.h"
#include "server/server_session.h"
#include "server/command.h"
#include "server/command_support.h"
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
        return serve(std::string{}, port, runtime);
    }

    bool FtpServer::serve(const std::string &host, int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return serve(host, port, *owned_runtime_);
    }

    bool FtpServer::serve(const std::string &host, int port, NetworkRuntime & runtime)
    {
        closing_ = false;

        bool bind_ok = host.empty() ? listener_.bind(port, runtime) : listener_.bind(host, port, runtime);
        if (!bind_ok) {
            LOG_ERROR("cant listen on {}:{}", host.empty() ? "0.0.0.0" : host, port);
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        listener_.set_connection_handler(
            [this](net::AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        accept_task_ = listener_.run_async();
        accept_task_.resume();

        if (owned_runtime_) {
            owned_runtime_->run();
            listener_.close();
            accept_task_ = {};
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
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            active_sessions_.insert(ptr_of(session));
        }

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

                ctx.append_output(format_ftp_response(res));
                ctx.flush();

                if (session->has_pending_transfer()) {
                    auto data_listener = session->take_data_listener();
                    auto *file_info = session->take_pending_file_info();
                    session->clear_pending_transfer();

                    if (data_listener && file_info) {
                        co_await data_transfer(ctx.runtime_view(), std::move(data_listener), file_info, ctx);
                    } else if (file_info) {
                        const auto active_addr = session->get_active_addr();
                        if (active_addr.has_value()) {
                            auto *conn = session->get_connection();
                            if (conn && conn->get_remote_address().get_ip() != active_addr->get_ip()) {
                                ctx.append_output("425 PORT address must match control connection peer.\r\n");
                                ctx.flush();
                                continue;
                            }
                            co_await active_data_transfer(ctx.runtime_view(), *active_addr, file_info, ctx);
                        } else {
                            ctx.append_output("425 No data connection mode selected.\r\n");
                            ctx.flush();
                        }
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

    coroutine::Task<void> FtpServer::active_data_transfer(
        coroutine::RuntimeView rv,
        const net::InetAddress & active_addr,
        FtpFileInfo * file_info,
        net::AsyncConnectionContext & control_ctx)
    {
        auto connect_result = co_await coroutine::async_connect(rv, active_addr.get_ip(), active_addr.get_port());
        if (connect_result.result != coroutine::ConnectResult::success || !connect_result.connection) {
            if (control_ctx.is_connected()) {
                control_ctx.append_output("425 Can't open data connection.\r\n");
                control_ctx.flush();
            }
            co_return;
        }

        net::AsyncConnectionContext data_ctx(connect_result.connection, rv);

        if (auto stream = std::dynamic_pointer_cast<StreamTransport>(connect_result.connection)) {
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
            (void)data_ctx.connection()->shutdown_write();
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
                if (file_info->fstream_) {
                    file_info->fstream_->flush();
                    file_info->fstream_->close();
                    file_info->fstream_.reset();
                }
                file_info->state_ = FileState::processed;
                transfer_ok = true;
            }
        }

        if (data_ctx.is_connected()) {
            co_await data_ctx.close_async();
        }

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
            (void)data_ctx.connection()->shutdown_write();
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
                if (file_info->fstream_) {
                    file_info->fstream_->flush();
                    file_info->fstream_->close();
                    file_info->fstream_.reset();
                }
                file_info->state_ = FileState::processed;
                transfer_ok = true;
            }
        }

        if (data_ctx.is_connected()) {
            co_await data_ctx.close_async();
        }

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

        std::size_t remaining = 0;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            active_sessions_.erase(session);
            remaining = active_sessions_.size();
        }
        LOG_INFO("ftp session closed, remaining sessions: {}", remaining);
    }

    void FtpServer::return_passive_port(FtpSession * session)
    {
        if (!session) {
            return;
        }
        auto passive_addr = session->get_passive_addr();
        if (passive_addr.has_value()) {
            ServerContext::get_instance()->remove_stream_port(passive_addr->get_port());
            session->clear_passive_addr();
        }
        session->clear_active_addr();
    }

    void FtpServer::quit()
    {
        if (closing_) {
            return;
        }

        closing_ = true;

        listener_.close();
        accept_task_ = {};

        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }
}
