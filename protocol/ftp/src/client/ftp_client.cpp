#include "client/ftp_client.h"
#include "coroutine/io_result.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/sync_wait.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/channel/channel.h"
#include "common/def.h"

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <vector>

namespace yuan::net::ftp
{
    namespace
    {
        InetAddress parse_pasv_endpoint(const std::string &body)
        {
            const auto left = body.find('(');
            const auto right = body.find(')');
            if (left == std::string::npos || right == std::string::npos || right <= left + 1) {
                return {};
            }
            std::string payload = body.substr(left + 1, right - left - 1);
            std::stringstream ss(payload);
            std::string item;
            std::vector<int> parts;
            while (std::getline(ss, item, ',')) {
                parts.push_back(std::stoi(item));
            }
            if (parts.size() != 6) {
                return {};
            }
            std::string ip = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]) + "." + std::to_string(parts[3]);
            return { ip, parts[4] * 256 + parts[5] };
        }
    }

    FtpClient::FtpClient() = default;

    FtpClient::~FtpClient()
    {
        quit();
    }

    bool FtpClient::connect(const std::string & ip, uint16_t port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        auto rv = owned_runtime_->runtime_view();
        return coroutine::sync_wait(rv, connect_async(rv, ip, port));
    }

    bool FtpClient::login(const std::string & username, const std::string & password)
    {
        if (!control_session_.is_connected()) {
            return false;
        }
        auto rv = control_session_.runtime_view();
        return coroutine::sync_wait(rv, login_async(username, password));
    }

    std::string FtpClient::list(const std::string & path)
    {
        if (!control_session_.is_connected()) {
            return {};
        }
        auto rv = control_session_.runtime_view();
        return coroutine::sync_wait(rv, list_async(path));
    }

    std::string FtpClient::nlist(const std::string & path)
    {
        if (!control_session_.is_connected()) {
            return {};
        }
        auto rv = control_session_.runtime_view();
        return coroutine::sync_wait(rv, nlist_async(path));
    }

    bool FtpClient::download(const std::string & remote_path, const std::string & local_path)
    {
        if (!control_session_.is_connected()) {
            return false;
        }
        auto rv = control_session_.runtime_view();
        return coroutine::sync_wait(rv, download_async(remote_path, local_path));
    }

    bool FtpClient::upload(const std::string & local_path, const std::string & remote_path)
    {
        if (!control_session_.is_connected()) {
            return false;
        }
        auto rv = control_session_.runtime_view();
        return coroutine::sync_wait(rv, upload_async(local_path, remote_path));
    }

    bool FtpClient::append(const std::string & local_path, const std::string & remote_path)
    {
        if (!control_session_.is_connected()) {
            return false;
        }
        auto rv = control_session_.runtime_view();
        return coroutine::sync_wait(rv, append_async(local_path, remote_path));
    }

    void FtpClient::quit()
    {
        if (control_session_.is_connected()) {
            control_session_.close();
        }
        connected_ = false;
        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }

    bool FtpClient::is_connected() const
    {
        return connected_ && control_session_.is_connected();
    }

    coroutine::Task<FtpClientResponse> FtpClient::read_response()
    {
        if (!pending_responses_.empty()) {
            auto resp = std::move(pending_responses_.front());
            pending_responses_.pop_front();
            co_return resp;
        }

        while (true) {
            auto responses = response_parser_.split_responses();
            if (!responses.empty()) {
                for (size_t i = 0; i + 1 < responses.size(); ++i) {
                    pending_responses_.push_back(std::move(responses[i]));
                }
                co_return std::move(responses.back());
            }

            auto read_result = co_await control_session_.read_async();
            if (read_result.status != coroutine::IoStatus::success) {
                co_return FtpClientResponse{ 0, {} };
            }
            response_parser_.set_buff(std::move(read_result.data));
        }
    }

    coroutine::Task<FtpClientResponse> FtpClient::send_command_and_read(const std::string & cmd)
    {
        if (!control_session_.is_connected()) {
            co_return FtpClientResponse{ 0, {} };
        }
        LOG_DEBUG("ftp client send: {}", cmd.substr(0, cmd.size() > 2 ? cmd.size() - 2 : cmd.size()));
        control_session_.context().append_output(cmd);
        control_session_.context().flush();
        co_return co_await read_response();
    }

    coroutine::Task<bool> FtpClient::connect_async(coroutine::RuntimeView rv, const std::string & ip, uint16_t port, uint32_t timeout_ms)
    {
        bool ok = co_await control_session_.connect_async(rv, ip, port, timeout_ms);
        if (!ok) {
            co_return false;
        }

        connected_ = true;

        auto welcome = co_await read_response();
        if (welcome.code_ != 220) {
            connected_ = false;
            control_session_.close();
            co_return false;
        }

        co_return true;
    }

    coroutine::Task<bool> FtpClient::login_async(const std::string & username, const std::string & password)
    {
        auto user_resp = co_await send_command_and_read("USER " + username + "\r\n");
        if (user_resp.code_ != 331 && user_resp.code_ != 230) {
            co_return false;
        }

        if (user_resp.code_ == 230) {
            co_return true;
        }

        auto pass_resp = co_await send_command_and_read("PASS " + password + "\r\n");
        co_return pass_resp.code_ == 230;
    }

    coroutine::Task<InetAddress> FtpClient::do_pasv()
    {
        auto resp = co_await send_command_and_read("PASV\r\n");
        if (resp.code_ != 227) {
            co_return InetAddress{};
        }
        co_return parse_pasv_endpoint(resp.body_);
    }

    coroutine::Task<net::AsyncConnectionContext> FtpClient::connect_data_channel(const InetAddress & data_addr, StreamMode mode)
    {
        auto rv = control_session_.runtime_view();

        auto connect_result = co_await coroutine::async_connect(rv, data_addr.get_ip(), data_addr.get_port());
        if (connect_result.result != coroutine::ConnectResult::success || !connect_result.connection) {
            co_return net::AsyncConnectionContext{};
        }

        auto data_conn = connect_result.connection;
        net::AsyncConnectionContext data_ctx(data_conn, rv);

        if (auto stream = std::dynamic_pointer_cast<StreamTransport>(data_conn)) {
            if (auto *channel = stream->stream_channel()) {
                if (mode == StreamMode::Receiver) {
                    channel->disable_write();
                    channel->enable_read();
                } else {
                    channel->disable_read();
                    channel->enable_write();
                }
                rv.update_channel(channel);
            }
        }

        co_return data_ctx;
    }

    coroutine::Task<FtpTransferResult> FtpClient::transfer_data(net::AsyncConnectionContext & data_ctx, FtpFileInfo & file_info)
    {
        bool transfer_ok = false;

        if (file_info.mode_ == StreamMode::Sender) {
            while (!file_info.is_completed()) {
                ::yuan::buffer::ByteBuffer buff(default_write_buff_size);
                int ret = file_info.read_file(default_write_buff_size, buff);
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
                if (file_info.is_completed()) {
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
                int ret = file_info.write_file(read_result.data);
                if (ret < 0) {
                    break;
                }
                if (file_info.is_completed()) {
                    transfer_ok = true;
                    break;
                }
            }
            if (file_info.file_size_ == 0 && !file_info.is_completed()) {
                file_info.state_ = FileState::processed;
                transfer_ok = true;
            }
        }

        co_await data_ctx.close_async();

        FtpTransferResult result;
        result.ok = transfer_ok;
        if (file_info.in_memory_ && file_info.mode_ == StreamMode::Receiver) {
            result.list_output = file_info.memory_content_;
        }
        co_return result;
    }

    coroutine::Task<std::string> FtpClient::list_async(const std::string & path)
    {
        auto data_addr = co_await do_pasv();
        if (data_addr.get_port() <= 0) {
            co_return{};
        }

        auto data_ctx = co_await connect_data_channel(data_addr, StreamMode::Receiver);
        if (!data_ctx.is_connected()) {
            co_return{};
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.type_ = FileType::directory;
        info.in_memory_ = true;
        info.ready_ = true;

        auto cmd = path.empty() ? "LIST\r\n" : "LIST " + path + "\r\n";
        auto cmd_resp = co_await send_command_and_read(cmd);
        if (cmd_resp.code_ != 150) {
            data_ctx.close();
            co_return{};
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);
        if (!transfer_result.ok) {
            co_return{};
        }

        auto final_resp = co_await read_response();
        if (final_resp.code_ != 226) {
            co_return{};
        }

        co_return transfer_result.list_output;
    }

    coroutine::Task<std::string> FtpClient::nlist_async(const std::string & path)
    {
        auto data_addr = co_await do_pasv();
        if (data_addr.get_port() <= 0) {
            co_return{};
        }

        auto data_ctx = co_await connect_data_channel(data_addr, StreamMode::Receiver);
        if (!data_ctx.is_connected()) {
            co_return{};
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.type_ = FileType::directory;
        info.in_memory_ = true;
        info.ready_ = true;

        auto cmd = path.empty() ? "NLST\r\n" : "NLST " + path + "\r\n";
        auto cmd_resp = co_await send_command_and_read(cmd);
        if (cmd_resp.code_ != 150) {
            data_ctx.close();
            co_return{};
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);
        if (!transfer_result.ok) {
            co_return{};
        }

        auto final_resp = co_await read_response();
        if (final_resp.code_ != 226) {
            co_return{};
        }

        co_return transfer_result.list_output;
    }

    coroutine::Task<bool> FtpClient::download_async(const std::string & remote_path, const std::string & local_path)
    {
        namespace fs = std::filesystem;

        std::size_t offset = 0;
        if (!local_path.empty() && fs::exists(local_path)) {
            offset = static_cast<std::size_t>(fs::file_size(local_path));
        }

        if (offset > 0) {
            auto rest_resp = co_await send_command_and_read("REST " + std::to_string(offset) + "\r\n");
            if (rest_resp.code_ != 350) {
                offset = 0;
            }
        }

        std::size_t file_size = 0;
        auto size_resp = co_await send_command_and_read("SIZE " + remote_path + "\r\n");
        if (size_resp.code_ == 213) {
            try
            {
                file_size = static_cast<std::size_t>(std::stoll(size_resp.body_));
            }
            catch (...)
            {
                co_return false;
            }
        }

        auto data_addr = co_await do_pasv();
        if (data_addr.get_port() <= 0) {
            co_return false;
        }

        auto data_ctx = co_await connect_data_channel(data_addr, StreamMode::Receiver);
        if (!data_ctx.is_connected()) {
            co_return false;
        }

        std::string resolved_local = local_path;
        if (resolved_local.empty()) {
            resolved_local = fs::path(remote_path).filename().generic_string();
        }
        const auto parent = fs::path(resolved_local).parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent);
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.origin_name_ = remote_path;
        info.dest_name_ = resolved_local;
        info.file_size_ = file_size;
        info.current_progress_ = offset;
        info.append_mode_ = offset > 0;
        info.ready_ = true;

        auto cmd_resp = co_await send_command_and_read("RETR " + remote_path + "\r\n");
        if (cmd_resp.code_ != 150) {
            data_ctx.close();
            co_return false;
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);

        auto final_resp = co_await read_response();
        co_return transfer_result.ok &&final_resp.code_ == 226;
    }

    coroutine::Task<bool> FtpClient::upload_async(const std::string & local_path, const std::string & remote_path)
    {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path)) {
            co_return false;
        }

        const std::size_t total = static_cast<std::size_t>(fs::file_size(local_path));

        auto allo_resp = co_await send_command_and_read("ALLO " + std::to_string(total) + "\r\n");
        if (allo_resp.code_ != 200 && allo_resp.code_ != 202) {
            co_return false;
        }

        auto data_addr = co_await do_pasv();
        if (data_addr.get_port() <= 0) {
            co_return false;
        }

        auto data_ctx = co_await connect_data_channel(data_addr, StreamMode::Sender);
        if (!data_ctx.is_connected()) {
            co_return false;
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Sender;
        info.origin_name_ = local_path;
        info.dest_name_ = remote_path;
        info.file_size_ = total;
        info.ready_ = true;

        auto cmd_resp = co_await send_command_and_read("STOR " + remote_path + "\r\n");
        if (cmd_resp.code_ != 150) {
            data_ctx.close();
            co_return false;
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);

        auto final_resp = co_await read_response();
        co_return transfer_result.ok &&final_resp.code_ == 226;
    }

    coroutine::Task<bool> FtpClient::append_async(const std::string & local_path, const std::string & remote_path)
    {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path)) {
            co_return false;
        }

        const std::size_t total = static_cast<std::size_t>(fs::file_size(local_path));

        auto allo_resp = co_await send_command_and_read("ALLO " + std::to_string(total) + "\r\n");
        if (allo_resp.code_ != 200 && allo_resp.code_ != 202) {
            co_return false;
        }

        auto data_addr = co_await do_pasv();
        if (data_addr.get_port() <= 0) {
            co_return false;
        }

        auto data_ctx = co_await connect_data_channel(data_addr, StreamMode::Sender);
        if (!data_ctx.is_connected()) {
            co_return false;
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Sender;
        info.origin_name_ = local_path;
        info.dest_name_ = remote_path;
        info.file_size_ = total;
        info.ready_ = true;

        auto cmd_resp = co_await send_command_and_read("APPE " + remote_path + "\r\n");
        if (cmd_resp.code_ != 150) {
            data_ctx.close();
            co_return false;
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);

        auto final_resp = co_await read_response();
        co_return transfer_result.ok &&final_resp.code_ == 226;
    }
}
