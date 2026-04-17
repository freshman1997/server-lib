#ifndef NET_FTP_FTP_CLIENT_H
#define NET_FTP_FTP_CLIENT_H
#include "client/context.h"
#include "coroutine/task.h"
#include "coroutine/io_result.h"
#include "net/async/async_client_session.h"
#include "net/runtime/network_runtime.h"
#include "client/response_parser.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <string>

namespace yuan::net::ftp
{
    struct FtpTransferResult
    {
        bool ok = false;
        std::string list_output;
    };

    class FtpClient
    {
    public:
        FtpClient();
        ~FtpClient();

        bool connect(const std::string &ip, uint16_t port);
        bool login(const std::string &username, const std::string &password);
        std::string list(const std::string &path = "");
        std::string nlist(const std::string &path = "");
        bool download(const std::string &remote_path, const std::string &local_path);
        bool upload(const std::string &local_path, const std::string &remote_path);
        bool append(const std::string &local_path, const std::string &remote_path);
        void quit();

        coroutine::Task<bool> connect_async(coroutine::RuntimeView rv, const std::string &ip, uint16_t port, uint32_t timeout_ms = 0);
        coroutine::Task<bool> login_async(const std::string &username, const std::string &password);
        coroutine::Task<std::string> list_async(const std::string &path = "");
        coroutine::Task<std::string> nlist_async(const std::string &path = "");
        coroutine::Task<bool> download_async(const std::string &remote_path, const std::string &local_path);
        coroutine::Task<bool> upload_async(const std::string &local_path, const std::string &remote_path);
        coroutine::Task<bool> append_async(const std::string &local_path, const std::string &remote_path);

        bool is_connected() const;

    private:
        coroutine::Task<FtpClientResponse> send_command_and_read(const std::string &cmd);
        coroutine::Task<FtpClientResponse> read_response();
        coroutine::Task<InetAddress> do_pasv();
        coroutine::Task<net::AsyncConnectionContext> connect_data_channel(const InetAddress &data_addr, StreamMode mode);
        coroutine::Task<FtpTransferResult> transfer_data(net::AsyncConnectionContext &data_ctx, FtpFileInfo &file_info);

    private:
        net::AsyncClientSession control_session_;
        FtpResponseParser response_parser_;
        std::deque<FtpClientResponse> pending_responses_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        bool connected_ = false;
    };
}
#endif
