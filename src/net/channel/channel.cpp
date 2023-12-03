#include "net/channel/channel.h"
namespace net
{
    Channel::Channel(int fd) : fd_(fd)
    {
        disable_all();
    }

    void Channel::on_event()
    {

    }
}