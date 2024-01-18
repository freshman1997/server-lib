#include <iostream>
#include <sys/epoll.h>
#include "net/channel/channel.h"
#include "net/handler/select_handler.h"

namespace net
{
    const int Channel::READ_EVENT = EPOLLIN | EPOLLPRI;
    const int Channel::WRITE_EVENT = EPOLLOUT;
    const int Channel::NONE_EVENT = 0;

    Channel::Channel(int fd) : fd_(fd), handler_(nullptr)
    {
        disable_all();
    }

    void Channel::on_event()
    {
        if (!handler_) {
            // TODO
            return;
        }

        if (events_ == NONE_EVENT) {
            return;
        }

        if (read_event_ & EPOLLIN || read_event_ & EPOLLERR || read_event_ & EPOLLHUP) {
            if (events_ & READ_EVENT && !dup_) {
                handler_->on_read_event();
            } else {
                std::cout << "=============> skip req \n";
            }
        }

        if (read_event_ & EPOLLOUT && events_ & WRITE_EVENT) {
            handler_->on_write_event();
        }
    }
}
