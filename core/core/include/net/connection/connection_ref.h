#ifndef __YUAN_NET_CONNECTION_CONNECTION_REF_H__
#define __YUAN_NET_CONNECTION_CONNECTION_REF_H__

#include <memory>

#include "net/connection/connection.h"

namespace yuan::net
{

    class ConnectionRef
    {
    public:
        ConnectionRef() = default;

        explicit ConnectionRef(Connection *connection) noexcept
            : connection_(connection)
        {
        }

        explicit ConnectionRef(std::shared_ptr<Connection> connection) noexcept
            : owner_(std::move(connection)),
              connection_(owner_.get())
        {
        }

        Connection *get() const noexcept
        {
            return connection_;
        }

        std::shared_ptr<Connection> owner() const
        {
            return owner_;
        }

        bool has_owner() const noexcept
        {
            return static_cast<bool>(owner_);
        }

        Connection &operator*() const noexcept
        {
            return *connection_;
        }

        Connection *operator->() const noexcept
        {
            return connection_;
        }

        explicit operator bool() const noexcept
        {
            return connection_ != nullptr;
        }

    private:
        std::shared_ptr<Connection> owner_;
        Connection *connection_ = nullptr;
    };

} // namespace yuan::net

#endif
