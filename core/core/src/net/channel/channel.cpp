#include "net/channel/channel.h"
#include "net/handler/select_handler.h"

#include <atomic>

namespace yuan::net
{
    namespace
    {
        uint64_t allocate_channel_generation() noexcept
        {
            static std::atomic<uint64_t> next_generation{ 1 };
            uint64_t generation = next_generation.fetch_add(1, std::memory_order_relaxed);
            if (generation == 0) {
                generation = next_generation.fetch_add(1, std::memory_order_relaxed);
            }
            return generation;
        }

        template <typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

    Channel::Channel() : Channel(0)
    {
    }

    Channel::Channel(int fd)
        : events_(NONE_EVENT),
          fd_(fd),
          revent_(NONE_EVENT),
          priority_(0),
          generation_(allocate_channel_generation()),
          handler_(nullptr),
          uses_handler_owner_(false)
    {
        disable_all();
    }

    void Channel::set_handler(SelectHandler *handler)
    {
        handler_ = handler;
        handler_owner_.reset();
        uses_handler_owner_ = false;
    }

    void Channel::set_handler(const std::weak_ptr<SelectHandler> &handler)
    {
        handler_owner_ = handler;
        uses_handler_owner_ = true;
        auto locked = handler_owner_.lock();
        handler_ = ptr_of(locked);
    }

    void Channel::clear_handler()
    {
        handler_ = nullptr;
        handler_owner_.reset();
        uses_handler_owner_ = false;
    }

    void Channel::on_event()
    {
        SelectHandler *handler = handler_;
        if (uses_handler_owner_) {
            auto locked = handler_owner_.lock();
            handler = ptr_of(locked);
            handler_ = handler;
            if (!handler) {
                return;
            }
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
