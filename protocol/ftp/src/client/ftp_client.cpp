#include "client/ftp_client.h"
#include "coroutine/io_result.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/sync_wait.h"
#include "net/async/async_listener_host.h"
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
#include <optional>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

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
                try {
                    const int value = std::stoi(item);
                    if (value < 0 || value > 255) {
                        return {};
                    }
                    parts.push_back(value);
                } catch (...) {
                    return {};
                }
            }
            if (parts.size() != 6) {
                return {};
            }
            std::string ip = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]) + "." + std::to_string(parts[3]);
            return { ip, parts[4] * 256 + parts[5] };
        }

        InetAddress parse_epsv_endpoint(const std::string &body, const std::string &fallback_ip)
        {
            const auto left = body.find('(');
            const auto right = body.find(')', left == std::string::npos ? 0 : left + 1);
            if (left == std::string::npos || right == std::string::npos || right <= left + 1) {
                return {};
            }

            const std::string payload = body.substr(left + 1, right - left - 1);
            if (payload.size() < 5) {
                return {};
            }

            const char delimiter = payload.front();
            if (payload[payload.size() - 1] != delimiter) {
                return {};
            }

            int delimiter_count = 0;
            for (char ch : payload) {
                if (ch == delimiter) {
                    ++delimiter_count;
                }
            }
            if (delimiter_count != 4) {
                return {};
            }

            const std::size_t third = payload.rfind(delimiter);
            if (third == std::string::npos || third == 0) {
                return {};
            }
            const std::size_t second = payload.rfind(delimiter, third - 1);
            if (second == std::string::npos) {
                return {};
            }

            const std::string port_text = payload.substr(second + 1, third - second - 1);
            if (port_text.empty()) {
                return {};
            }

            try {
                const int port = std::stoi(port_text);
                if (port <= 0 || port > 65535) {
                    return {};
                }
                return { fallback_ip, port };
            } catch (...) {
                return {};
            }
        }

        std::string format_port_command(const std::string &ip, int port)
        {
            std::stringstream ss(ip);
            std::string seg;
            std::vector<int> octets;
            while (std::getline(ss, seg, '.')) {
                try {
                    int value = std::stoi(seg);
                    if (value < 0 || value > 255) {
                        return {};
                    }
                    octets.push_back(value);
                } catch (...) {
                    return {};
                }
            }
            if (octets.size() != 4 || port <= 0 || port > 65535) {
                return {};
            }
            const int p1 = port / 256;
            const int p2 = port % 256;
            return "PORT " + std::to_string(octets[0]) + "," +
                   std::to_string(octets[1]) + "," +
                   std::to_string(octets[2]) + "," +
                   std::to_string(octets[3]) + "," +
                   std::to_string(p1) + "," +
                   std::to_string(p2) + "\r\n";
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
        active_listener_.reset();
        if (control_session_.is_connected()) {
            auto rv = control_session_.runtime_view();
            (void)coroutine::sync_wait(rv, send_command_and_read("QUIT\r\n"));
        }
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

    coroutine::Task<InetAddress> FtpClient::do_epsv()
    {
        auto resp = co_await send_command_and_read("EPSV\r\n");
        if (resp.code_ != 229) {
            co_return InetAddress{};
        }

        std::string control_ip = "127.0.0.1";
        if (auto *conn = control_session_.native_handle()) {
            control_ip = conn->get_remote_address().get_ip();
        }
        co_return parse_epsv_endpoint(resp.body_, control_ip);
    }

    coroutine::Task<bool> FtpClient::do_eprt(const std::string & ip, uint16_t port)
    {
        const std::string af = ip.find(':') != std::string::npos ? "2" : "1";
        const std::string cmd = "EPRT |" + af + "|" + ip + "|" + std::to_string(port) + "|\r\n";
        auto resp = co_await send_command_and_read(cmd);
        co_return resp.code_ == 200;
    }

    coroutine::Task<InetAddress> FtpClient::open_data_channel(StreamMode mode)
    {
        if (data_mode_ == DataMode::active_only) {
            co_return InetAddress{};
        }

        if (data_mode_ == DataMode::auto_select || data_mode_ == DataMode::passive_only) {
            auto epsv_addr = co_await do_epsv();
            if (epsv_addr.get_port() > 0) {
                co_return epsv_addr;
            }

            auto pasv_addr = co_await do_pasv();
            if (pasv_addr.get_port() > 0) {
                co_return pasv_addr;
            }
        }

        co_return InetAddress{};
    }

    coroutine::Task<bool> FtpClient::prepare_active_data_listener()
    {
        active_listener_.reset();

        if (!control_session_.is_connected()) {
            co_return false;
        }

        auto *control_conn = control_session_.native_handle();
        if (!control_conn) {
            co_return false;
        }

        const std::string local_ip = control_conn->get_local_address().get_ip();
        if (local_ip.empty()) {
            co_return false;
        }

        if (!owned_runtime_) {
            co_return false;
        }

        active_listener_ = std::make_unique<net::AsyncListenerHost>();
        if (!active_listener_->bind(local_ip, 0, *owned_runtime_)) {
            active_listener_.reset();
            co_return false;
        }

        auto *acceptor = active_listener_->acceptor();
        auto *listener_channel = acceptor ? acceptor->listener_channel() : nullptr;
        if (!listener_channel) {
            active_listener_->close();
            active_listener_.reset();
            co_return false;
        }

        int bound_port = 0;

        // best effort: derive port from acceptor fd
        {
            sockaddr_storage addr{};
#ifdef _WIN32
            int len = static_cast<int>(sizeof(addr));
#else
            socklen_t len = sizeof(addr);
#endif
            if (::getsockname(listener_channel->get_fd(), reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
                net::InetAddress bound(addr);
                bound_port = bound.get_port();
            }
        }

        if (bound_port <= 0) {
            active_listener_->close();
            active_listener_.reset();
            co_return false;
        }

        bool ok = co_await do_eprt(local_ip, static_cast<uint16_t>(bound_port));
        if (!ok) {
            const auto cmd = format_port_command(local_ip, bound_port);
            if (cmd.empty()) {
                active_listener_->close();
                active_listener_.reset();
                co_return false;
            }
            auto resp = co_await send_command_and_read(cmd);
            if (resp.code_ != 200) {
                active_listener_->close();
                active_listener_.reset();
                co_return false;
            }
        }

        co_return true;
    }

    coroutine::Task<net::AsyncConnectionContext> FtpClient::accept_prepared_active_data_channel(StreamMode mode)
    {
        if (!active_listener_) {
            co_return net::AsyncConnectionContext{};
        }

        auto rv = control_session_.runtime_view();

        auto data_conn = co_await active_listener_->accept_async();
        if (!data_conn) {
            active_listener_->close();
            active_listener_.reset();
            co_return net::AsyncConnectionContext{};
        }

        net::AsyncConnectionContext data_ctx(data_conn, rv);
        data_ctx.install_default_handler();

        active_listener_->close();
        active_listener_.reset();
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

    namespace
    {
        struct FtpDataPrep
        {
            bool passive = false;
            InetAddress passive_addr;
            bool active_ready = false;
        };
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
            (void)data_ctx.connection()->shutdown_write();
        } else {
            while (true) {
                auto read_result = co_await data_ctx.read_async(0, false);
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
                if (read_result.data.readable_bytes() == 0) {
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
        FtpDataPrep prep;
        if (data_mode_ != DataMode::active_only) {
            prep.passive_addr = co_await open_data_channel(StreamMode::Receiver);
            prep.passive = prep.passive_addr.get_port() > 0;
        }
        if (!prep.passive && data_mode_ != DataMode::passive_only) {
            prep.active_ready = co_await prepare_active_data_listener();
        }
        if (!prep.passive && !prep.active_ready) {
            co_return{};
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.type_ = FileType::directory;
        info.in_memory_ = true;
        info.ready_ = true;

        auto cmd = path.empty() ? "LIST\r\n" : "LIST " + path + "\r\n";
        auto cmd_resp = co_await send_command_and_read(cmd);
        if (cmd_resp.code_ != 150 && cmd_resp.code_ != 125) {
            active_listener_.reset();
            co_return{};
        }

        net::AsyncConnectionContext data_ctx;
        if (prep.passive) {
            data_ctx = co_await connect_data_channel(prep.passive_addr, StreamMode::Receiver);
        } else {
            data_ctx = co_await accept_prepared_active_data_channel(StreamMode::Receiver);
        }
        if (!data_ctx.is_connected()) {
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
        FtpDataPrep prep;
        if (data_mode_ != DataMode::active_only) {
            prep.passive_addr = co_await open_data_channel(StreamMode::Receiver);
            prep.passive = prep.passive_addr.get_port() > 0;
        }
        if (!prep.passive && data_mode_ != DataMode::passive_only) {
            prep.active_ready = co_await prepare_active_data_listener();
        }
        if (!prep.passive && !prep.active_ready) {
            co_return{};
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.type_ = FileType::directory;
        info.in_memory_ = true;
        info.ready_ = true;

        auto cmd = path.empty() ? "NLST\r\n" : "NLST " + path + "\r\n";
        auto cmd_resp = co_await send_command_and_read(cmd);
        if (cmd_resp.code_ != 150 && cmd_resp.code_ != 125) {
            active_listener_.reset();
            co_return{};
        }

        net::AsyncConnectionContext data_ctx;
        if (prep.passive) {
            data_ctx = co_await connect_data_channel(prep.passive_addr, StreamMode::Receiver);
        } else {
            data_ctx = co_await accept_prepared_active_data_channel(StreamMode::Receiver);
        }
        if (!data_ctx.is_connected()) {
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

        FtpDataPrep prep;
        if (data_mode_ != DataMode::active_only) {
            prep.passive_addr = co_await open_data_channel(StreamMode::Receiver);
            prep.passive = prep.passive_addr.get_port() > 0;
        }
        if (!prep.passive && data_mode_ != DataMode::passive_only) {
            prep.active_ready = co_await prepare_active_data_listener();
        }
        if (!prep.passive && !prep.active_ready) {
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
        if (cmd_resp.code_ != 150 && cmd_resp.code_ != 125) {
            active_listener_.reset();
            co_return false;
        }

        net::AsyncConnectionContext data_ctx;
        if (prep.passive) {
            data_ctx = co_await connect_data_channel(prep.passive_addr, StreamMode::Receiver);
        } else {
            data_ctx = co_await accept_prepared_active_data_channel(StreamMode::Receiver);
        }
        if (!data_ctx.is_connected()) {
            co_return false;
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);

        auto final_resp = co_await read_response();
        co_return transfer_result.ok && final_resp.code_ == 226;
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

        FtpDataPrep prep;
        if (data_mode_ != DataMode::active_only) {
            prep.passive_addr = co_await open_data_channel(StreamMode::Sender);
            prep.passive = prep.passive_addr.get_port() > 0;
        }
        if (!prep.passive && data_mode_ != DataMode::passive_only) {
            prep.active_ready = co_await prepare_active_data_listener();
        }
        if (!prep.passive && !prep.active_ready) {
            co_return false;
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Sender;
        info.origin_name_ = local_path;
        info.dest_name_ = remote_path;
        info.file_size_ = total;
        info.ready_ = true;

        auto cmd_resp = co_await send_command_and_read("STOR " + remote_path + "\r\n");
        if (cmd_resp.code_ != 150 && cmd_resp.code_ != 125) {
            active_listener_.reset();
            co_return false;
        }

        net::AsyncConnectionContext data_ctx;
        if (prep.passive) {
            data_ctx = co_await connect_data_channel(prep.passive_addr, StreamMode::Sender);
        } else {
            data_ctx = co_await accept_prepared_active_data_channel(StreamMode::Sender);
        }
        if (!data_ctx.is_connected()) {
            co_return false;
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);

        auto final_resp = co_await read_response();
        co_return transfer_result.ok && final_resp.code_ == 226;
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

        FtpDataPrep prep;
        if (data_mode_ != DataMode::active_only) {
            prep.passive_addr = co_await open_data_channel(StreamMode::Sender);
            prep.passive = prep.passive_addr.get_port() > 0;
        }
        if (!prep.passive && data_mode_ != DataMode::passive_only) {
            prep.active_ready = co_await prepare_active_data_listener();
        }
        if (!prep.passive && !prep.active_ready) {
            co_return false;
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Sender;
        info.origin_name_ = local_path;
        info.dest_name_ = remote_path;
        info.file_size_ = total;
        info.ready_ = true;

        auto cmd_resp = co_await send_command_and_read("APPE " + remote_path + "\r\n");
        if (cmd_resp.code_ != 150 && cmd_resp.code_ != 125) {
            active_listener_.reset();
            co_return false;
        }

        net::AsyncConnectionContext data_ctx;
        if (prep.passive) {
            data_ctx = co_await connect_data_channel(prep.passive_addr, StreamMode::Sender);
        } else {
            data_ctx = co_await accept_prepared_active_data_channel(StreamMode::Sender);
        }
        if (!data_ctx.is_connected()) {
            co_return false;
        }

        auto transfer_result = co_await transfer_data(data_ctx, info);

        auto final_resp = co_await read_response();
        co_return transfer_result.ok && final_resp.code_ == 226;
    }
}
