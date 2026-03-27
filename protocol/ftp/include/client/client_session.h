#ifndef __NET_FTP_CLIENT_CLIENT_SESSION_H__
#define __NET_FTP_CLIENT_CLIENT_SESSION_H__
#include "context.h"
#include "response_parser.h"
#include "common/session.h"

namespace yuan::net::ftp
{
    class FtpApp;

    class ClientFtpSession : public FtpSession
    {
    public:
        ClientFtpSession(Connection *conn, FtpApp *entry, bool keepUtilSent = false);
        ~ClientFtpSession();

        virtual void on_opened(FtpFileStreamSession *fs);
        virtual void on_read(Connection *conn);
        virtual void on_completed(FtpFileStreamSession *fs);

        bool login(const std::string &username, const std::string &password);
        bool list(const std::string &path = "");
        bool nlist(const std::string &path = "");
        bool download(const std::string &remote_path, const std::string &local_path);
        bool upload(const std::string &local_path, const std::string &remote_path);
        bool append(const std::string &local_path, const std::string &remote_path);

        const ClientContext &get_client_context() const { return client_context_; }

    private:
        void handle_response(const FtpClientResponse &response);
        bool begin_pasv_flow(ClientPendingAction action, const std::string &remote_path, const std::string &local_path = "");
        bool prepare_list_receiver();
        bool prepare_download_file();
        bool prepare_upload_file();

    private:
        ClientContext client_context_;
        FtpResponseParser response_parser_;
    };
}

#endif
