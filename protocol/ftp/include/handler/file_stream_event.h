#ifndef NET_FTP_HANDLER_FILE_STREAM_H
#define NET_FTP_HANDLER_FILE_STREAM_H

namespace yuan::net::ftp 
{
    class FtpFileStreamSession;

    class FtpFileStreamEvent
    {
    public:
        virtual void on_opened(FtpFileStreamSession *fs) = 0;

        virtual void on_connect_timeout(FtpFileStreamSession *fs) = 0;

        virtual void on_start(FtpFileStreamSession *fs) = 0;

        virtual void on_error(FtpFileStreamSession *fs) = 0;

        virtual void on_completed(FtpFileStreamSession *fs) = 0;

        virtual void on_closed(FtpFileStreamSession *fs) = 0;

        virtual void on_idle_timeout(FtpFileStreamSession *fs) = 0;
    };
}

#endif