#ifndef __CHANNEL_H__
#define __CHANNEL_H__

namespace net
{
    class Channel
    {
    public:
        enum class Oper
        {
            init = -1,
            add,
            free,
        };

    public:
        Channel(int fd);

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

        void enable_read();
        void enable_write();
        void disable_all();

        void set_read_event(int ev)
        {
            read_event_ = ev;
        }

    private:
        int events_;
        int read_event_;
        int fd_;
        Oper oper = Oper::init;
    };
}


#endif