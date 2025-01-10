#include "net/channel/channel.h"
#include "net/handler/select_handler.h"

namespace yuan::net
{
    const int Channel::READ_EVENT = 0x1;
    const int Channel::WRITE_EVENT = 0x2;
    const int Channel::EXCEP_EVENT = 0x4;
    const int Channel::NONE_EVENT = 0;

    Channel::Channel() : Channel(0)
    {
    }

    Channel::Channel(int fd) : fd_(fd), handler_(nullptr),events_(NONE_EVENT), revent_(NONE_EVENT), priority_(0)
    {
        disable_all();
    }

    void Channel::set_handler(SelectHandler *handler)
    {
        handler_ = handler;
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

        if (revent_ & READ_EVENT && events_ & READ_EVENT) {
            handler_->on_read_event();
            if (handler_ == nullptr) {
                delete this;
                return;
            }
        }

        if (revent_ & WRITE_EVENT && events_ & WRITE_EVENT) {
            handler_->on_write_event();
            if (handler_ == nullptr) {
                delete this;
                return;
            }
        }
    }
}
