#ifndef NET_FTP_FTP_SERVER_H
#define NET_FTP_FTP_SERVER_H
#include <memory>
#include <unordered_set>

#include "coroutine/task.h"
#include "handler/ftp_app.h"
#include "net/async/async_connection_context.h"
#include "net/async/async_listener_host.h"
#include "net/runtime/network_runtime.h"

namespace yuan::net::ftp
{
    class FtpSession;
    class FtpFileInfo;

    class FtpServer : public FtpApp
    {
    public:
        FtpServer();
        ~FtpServer();

        bool serve(int port);
        bool serve(int port, NetworkRuntime &runtime);

    public:
        bool is_ok() override;
        NetworkRuntime *get_runtime() override;
        void on_session_closed(FtpSession *session) override;
        void quit() override;

    private:
        coroutine::Task<void> handle_connection(net::AsyncConnectionContext ctx);
        coroutine::Task<void> data_transfer(
            coroutine::RuntimeView rv,
            std::unique_ptr<net::AsyncListenerHost> listener,
            FtpFileInfo *file_info,
            net::AsyncConnectionContext &control_ctx);
        void return_passive_port(FtpSession *session);

    private:
        net::AsyncListenerHost listener_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        std::unordered_set<FtpSession *> active_sessions_;
        bool closing_ = false;
    };
}

#endif
