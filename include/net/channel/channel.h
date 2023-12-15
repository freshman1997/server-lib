#ifndef __CHANNEL_H__
#define __CHANNEL_H__

namespace net
{
    class SelectHandler;

    class Channel
    {
    public:
        enum class Oper : char
        {
            init = -1,
            add,
            free,
        };

    public:
        Channel(int fd);

        void on_event();

        int get_fd() const 
        {
            return fd_;
        }

        int get_events() const 
        {
            return events_;
        }

        Oper get_oper() const 
        {
            return oper;
        }

        void set_oper(Oper op) 
        {
            oper = op;
        }

        bool has_events()
        {
            return false;
        }

        void enable_read()
        {
            events_ |= READ_EVENT;
        }

        void enable_write()
        {
            events_ |= WRITE_EVENT;
            
        }

        void disable_all()
        {
            events_ = NONE_EVENT;
        }

        void set_read_event(int ev)
        {
            read_event_ = ev;
        }

        void set_handler(SelectHandler *handler)
        {
            handler_ = handler;
        }

    private:
        static const int READ_EVENT;
        static const int WRITE_EVENT;
        static const int NONE_EVENT;

    private:
        int events_;
        int read_event_;
        int fd_;
        Oper oper = Oper::init;
        SelectHandler *handler_;
    };
}


#endif