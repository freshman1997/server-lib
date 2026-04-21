#include "net/channel/channel.h"
#include "net/handler/select_handler.h"

namespace yuan::net
{
    namespace
    {
        template <typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

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
        handler_owner_.reset();
    }

    void Channel::set_handler(const std::weak_ptr<SelectHandler> &handler)
    {
        handler_owner_ = handler;
        auto locked = handler_owner_.lock();
        handler_ = ptr_of(locked);
    }

    void Channel::clear_handler()
    {
        handler_ = nullptr;
        handler_owner_.reset();
    }

    void Channel::on_event()
    {
        SelectHandler *handler = handler_;
        if (!handler_owner_.expired()) {
            auto locked = handler_owner_.lock();
            handler = ptr_of(locked);
            handler_ = handler;
        }

        if (!handler) {
            return;
        }

        if (events_ == NONE_EVENT) {
            return;
        }

        if (revent_ & READ_EVENT && events_ & READ_EVENT) {
            handler->on_read_event();
        }

        if (revent_ & WRITE_EVENT && events_ & WRITE_EVENT) {
            handler->on_write_event();
        }
    }
}
