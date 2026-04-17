#ifndef NET_FTP_HANDLER_ENTRY_H
#define NET_FTP_HANDLER_ENTRY_H
namespace yuan::net
{
    class NetworkRuntime;
}

namespace yuan::net::ftp
{
    class FtpSession;

    class FtpApp
    {
    public:
        virtual bool is_ok() = 0;

        virtual NetworkRuntime *get_runtime() = 0;

        virtual void on_session_closed(FtpSession *session) = 0;

        virtual void quit() = 0;
    };
}

#endif
