#ifndef __CHANNEL_H__
#define __CHANNEL_H__

#include <memory>

namespace yuan::net
{
    class SelectHandler;

    class Channel
    {
    public:
        Channel();

        Channel(int fd);

        void on_event();

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

        bool has_events() const
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

        void set_handler(SelectHandler *handler);

        void set_handler(const std::weak_ptr<SelectHandler> &handler);

        void clear_handler();

        bool has_handler() const noexcept
        {
            return handler_ != nullptr || !handler_owner_.expired();
        }

        void set_revent(int revent)
        {
            revent_ = revent;
        }

        void set_priority(int priority)
        {
            priority_ = priority;
        }

        int get_priority() const
        {
            return priority_;
        }

        uint64_t generation() const noexcept
        {
            return generation_;
        }

        void set_generation(uint64_t generation) noexcept
        {
            generation_ = generation == 0 ? 1 : generation;
        }

        void bump_generation() noexcept
        {
            ++generation_;
            if (generation_ == 0) {
                generation_ = 1;
            }
        }

    public:
        static constexpr int READ_EVENT = 0b001;
        static constexpr int WRITE_EVENT = 0b010;
        static constexpr int EXCEP_EVENT = 0b100;
        static constexpr int NONE_EVENT = 0;

    private:
        int events_;
        int fd_;
        int revent_;
        int priority_;
        uint64_t generation_;
        SelectHandler *handler_;
        std::weak_ptr<SelectHandler> handler_owner_;
    };
}

#endif
