#ifndef __NET_FTP_COMMON_FILE_STREAM_SESSION_H__
#define __NET_FTP_COMMON_FILE_STREAM_SESSION_H__
#include "../../base/handler/connection_handler.h"
#include "../../../timer/timer_task.h"
#include "def.h"

namespace net 
{
    class Connection;
}

namespace net::ftp 
{
    class FtpSession;

    class FtpFileStreamSession : public timer::TimerTask, public ConnectionHandler
    {
    public:
        FtpFileStreamSession(FtpSession *session);
        ~FtpFileStreamSession();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        virtual void on_timer(timer::Timer *timer);

    public:
        Connection * get_connection();

        std::size_t get_write_buff_size();

        void set_write_buff_size(std::size_t sz);

        void set_work_file(FtpFileInfo *info);

        FtpFileInfo * get_work_file();

        void quit();

        FileSteamState get_state();

    private:
        FileSteamState state_;
        std::size_t write_buff_size_;
        uint32_t last_active_time_;
        FtpFileInfo *current_file_info_;
        Connection *conn_;
        timer::Timer *conn_timer_;
        FtpSession *session_;
    };
}

#endif