#ifndef __CHANNEL_H__
#define __CHANNEL_H__

namespace net
{
    class Channel
    {
    public:
        Channel(int fd);

        int get_fd() const 
        {
            return fd_;
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
    };
}


#endif