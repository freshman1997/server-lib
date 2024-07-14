#ifndef _NET_FTP_CLIENT_FILE_STREAM_H__
#define _NET_FTP_CLIENT_FILE_STREAM_H__
#include "../../base/handler/connection_handler.h"
#include "../common/def.h"
#include "../handler/file_stream_event.h"
#include "../../../timer/timer_task.h"
#include "../../../timer/timer_manager.h"
#include "../../base/socket/inet_address.h"

namespace net::ftp 
{
    class FtpApp;

    class FtpFileStream : public ConnectionHandler, public timer::TimerTask
    {
    public:
        FtpFileStream(const InetAddress &addr, StreamMode mode, FtpApp *entry);
        ~FtpFileStream();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        virtual void on_timer(timer::Timer *timer);

    public:
        void set_event_handler(FtpFileStreamEvent *eventHandler);

        void set_work_file(FileInfo *info);

        FileInfo * get_work_file();

        FileSteamState get_state();

        Connection * get_cur_connection();

        bool start();

        void quit();
        
    private:
        bool serve();

        bool connect();

    private:
        StreamMode mode_;
        FileSteamState state_;
        InetAddress addr_;
        FileInfo *current_file_info_;
        FtpFileStreamEvent *event_handler_;
        timer::Timer *conn_timer_;
        Connection *conn_;
        FtpApp *entry_;
    };
}

#endif