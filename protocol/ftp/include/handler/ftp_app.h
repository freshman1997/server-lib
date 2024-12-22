#ifndef __NET_FTP_HANDLER_ENTRY_H__
#define __NET_FTP_HANDLER_ENTRY_H__
namespace yuan::net 
{
    class EventHandler;
}

namespace yuan::timer 
{
    class TimerManager;
}

namespace yuan::net::ftp 
{
    class FtpSession;

    class FtpApp
    {
    public:
        virtual bool is_ok() = 0;

        virtual timer::TimerManager * get_timer_manager() = 0;

        virtual EventHandler * get_event_handler() = 0;

        virtual void on_session_closed(FtpSession *session) = 0;

        virtual void quit() = 0;
    };
}

#endif