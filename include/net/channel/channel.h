#ifndef __CHANNEL_H__
#define __CHANNEL_H__

namespace net
{
    class SelectHandler;

    class Channel
    {
    public:
        Channel();

        Channel(int fd);

        void on_event(int event);

        void set_fd(int fd)
        {
            fd_ = fd;
        }

        int get_fd() const 
        {
            return fd_;
        }

        int get_events() const 
        {
            return events_;
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

        void disable_read()
        {
            events_ &= ~READ_EVENT;
        }

        void disable_write()
        {
            events_ &= ~WRITE_EVENT;
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
        SelectHandler *handler_;
    };
}


#endif