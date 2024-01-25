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

        void on_event(int event);

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
            return oper_;
        }

        void set_oper(Oper op) 
        {
            oper_ = op;
        }

        bool has_events()
        {
            return events_ != NONE_EVENT;
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

        void set_handler(SelectHandler *handler)
        {
            handler_ = handler;
        }

        void set_new_fd(int new_fd) 
        {
            fd_ = new_fd;
        }

    public:
        static const int READ_EVENT;
        static const int WRITE_EVENT;
        static const int EXCEP_EVENT;
        static const int NONE_EVENT;

    private:
        int events_;
        int fd_;
        Oper oper_ = Oper::init;
        SelectHandler *handler_;
    };
}


#endif