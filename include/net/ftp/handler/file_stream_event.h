#ifndef __NET_FTP_HANDLER_FILE_STREAM_H__
#define __NET_FTP_HANDLER_FILE_STREAM_H__

namespace net::ftp 
{
    class FtpFileStream;

    class FtpFileStreamEvent
    {
    public:
        virtual void on_opened(FtpFileStream *fs) = 0;

        virtual void on_connect_timeout(FtpFileStream *fs) = 0;

        virtual void on_start(FtpFileStream *fs) = 0;

        virtual void on_error(FtpFileStream *fs) = 0;

        virtual void on_completed(FtpFileStream *fs) = 0;

        virtual void on_closed(FtpFileStream *fs) = 0;

        virtual void on_idle_timeout(FtpFileStream *fs) = 0;
    };
}

#endif