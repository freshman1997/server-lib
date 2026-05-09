#include "coroutine/runtime.h"
#include "net/connection/connection_handle.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"

#include <iostream>
#include <memory>

namespace
{
    class FakeConnection final : public yuan::net::Connection
    {
    public:
        yuan::net::ConnectionState get_connection_state() const override
        {
            return yuan::net::ConnectionState::connected;
        }

        bool is_connected() const override
        {
            return true;
        }

        const yuan::net::InetAddress &get_remote_address() const override
        {
            return address_;
        }

        const yuan::net::InetAddress &get_local_address() const override
        {
            return address_;
        }

        void write(const yuan::buffer::ByteBuffer &) override {}
        void write_and_flush(const yuan::buffer::ByteBuffer &) override {}
        void flush() override {}
        void abort() override {}
        void close() override {}

        void set_connection_handler(std::shared_ptr<yuan::net::ConnectionHandler> handler) override
        {
            handler_ = std::move(handler);
        }

        yuan::net::ConnectionHandler *get_connection_handler() const override
        {
            return handler_.get();
        }

        std::shared_ptr<yuan::net::ConnectionHandler> get_connection_handler_owner() const override
        {
            return handler_;
        }

        void set_ssl_handler(std::shared_ptr<yuan::net::SSLHandler>) override {}
        void on_read_event() override {}
        void on_write_event() override {}
        void set_event_handler(yuan::net::EventHandler *) override {}

    private:
        yuan::net::InetAddress address_{"127.0.0.1", 0};
        std::shared_ptr<yuan::net::ConnectionHandler> handler_;
    };
}

int main()
{
    auto connection = std::make_shared<FakeConnection>();
    const auto initial_use_count = connection.use_count();

    yuan::net::ConnectionHandle handle(connection);
    if (!handle || handle.get() != connection.get() || handle.shared() != connection) {
        std::cerr << "connection handle should keep shared owner\n";
        return 1;
    }

    if (connection.use_count() != initial_use_count + 1) {
        std::cerr << "connection handle should increase owner count\n";
        return 1;
    }

    yuan::net::ConnectionView view(*connection);
    if (!view || view.get() != connection.get() || !view->is_connected()) {
        std::cerr << "connection view should reference current stack connection\n";
        return 1;
    }

    connection.reset();
    if (!handle || !handle->is_connected()) {
        std::cerr << "connection handle should keep connection alive after caller releases shared_ptr\n";
        return 1;
    }

    yuan::coroutine::RuntimeView runtime;
    yuan::buffer::ByteBuffer buffer;
    auto read_awaiter = runtime.read(handle, 1);
    auto write_awaiter = runtime.write(handle, buffer, 1);
    auto flush_awaiter = runtime.flush(handle, 1);
    auto close_awaiter = runtime.close(handle);
    (void)read_awaiter;
    (void)write_awaiter;
    (void)flush_awaiter;
    (void)close_awaiter;

    std::cout << "connection handle test passed\n";
    return 0;
}
