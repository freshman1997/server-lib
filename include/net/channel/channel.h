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

private:
    int fd_;
};

}


#endif