#ifndef __TCP_SOCKET_HANDLER_H__
#define __TCP_SOCKET_HANDLER_H__

#include <functional>
#include <memory>

namespace yuan::net
{
    class Connection;

    template<typename HandlerT>
    std::shared_ptr<HandlerT> make_non_owning_handler(HandlerT *handler)
    {
        return std::shared_ptr<HandlerT>(handler, [](HandlerT *) {});
    }

    template<typename HandlerT>
    std::shared_ptr<HandlerT> make_non_owning_handler(HandlerT &handler)
    {
        return make_non_owning_handler(&handler);
    }

    template<typename OwnerT, typename HandlerT>
    std::shared_ptr<HandlerT> make_aliasing_handler(const std::shared_ptr<OwnerT> &owner, HandlerT *handler)
    {
        return owner ? std::shared_ptr<HandlerT>(owner, handler) : std::shared_ptr<HandlerT>(handler, [](HandlerT *) {});
    }

    class ConnectionHandler
    {
    public:
        virtual ~ConnectionHandler() = default;

        virtual void on_connected(Connection &conn) = 0;
        virtual void on_error(Connection &conn) = 0;
        virtual void on_read(Connection &conn) = 0;
        virtual void on_write(Connection &conn) = 0;
        virtual void on_close(Connection &conn) = 0;
        virtual void on_input_shutdown(Connection &conn) { (void)conn; }

        virtual bool is_input_shutdown() const { return false; }
    };

    template<typename BaseHandler>
    class FilteredConnectionHandler : public BaseHandler
    {
    public:
        using filter_func = std::function<bool(Connection *)>;

        explicit FilteredConnectionHandler(filter_func read_filter = nullptr,
                                           filter_func write_filter = nullptr)
            : read_filter_(std::move(read_filter)),
              write_filter_(std::move(write_filter))
        {
        }

        void set_read_filter(filter_func fn) { read_filter_ = std::move(fn); }
        void set_write_filter(filter_func fn) { write_filter_ = std::move(fn); }

    protected:
        filter_func read_filter_;
        filter_func write_filter_;
    };
}

#endif
