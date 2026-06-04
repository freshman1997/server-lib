#ifndef __YUAN_NET_CONNECTION_CONNECTION_HANDLE_H__
#define __YUAN_NET_CONNECTION_CONNECTION_HANDLE_H__

#include <cassert>
#include <memory>

#include "net/connection/connection.h"

namespace yuan::net
{

    class ConnectionHandle
    {
    public:
        ConnectionHandle() = default;

        explicit ConnectionHandle(std::shared_ptr<Connection> connection) noexcept
            : connection_(std::move(connection))
        {
        }

        Connection *get() const noexcept
        {
            return connection_.get();
        }

        const std::shared_ptr<Connection> &shared() const noexcept
        {
            return connection_;
        }

        Connection &operator*() const noexcept
        {
            assert(connection_);
            return *connection_;
        }

        Connection *operator->() const noexcept
        {
            assert(connection_);
            return connection_.get();
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(connection_);
        }

    private:
        std::shared_ptr<Connection> connection_;
    };

    // Non-owning view of a Connection. The caller must guarantee that the
    // underlying Connection remains alive for the entire lifetime of this view.
    // Do not store a ConnectionView across coroutine suspension points or
    // thread boundaries where the Connection may be destroyed asynchronously.
    // Use ConnectionHandle (shared_ptr-backed) for lifetime-safe references.
    class ConnectionView
    {
    public:
        explicit ConnectionView(Connection &connection) noexcept
            : connection_(&connection)
        {
        }

        Connection *get() const noexcept
        {
            return connection_;
        }

        Connection &operator*() const noexcept
        {
            assert(connection_);
            return *connection_;
        }

        Connection *operator->() const noexcept
        {
            assert(connection_);
            return connection_;
        }

        explicit operator bool() const noexcept
        {
            return connection_ != nullptr;
        }

    private:
        Connection *connection_ = nullptr;
    };

} // namespace yuan::net

#endif
