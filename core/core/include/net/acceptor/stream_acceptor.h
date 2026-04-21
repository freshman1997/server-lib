#ifndef __YUAN_NET_ACCEPTOR_STREAM_ACCEPTOR_H__
#define __YUAN_NET_ACCEPTOR_STREAM_ACCEPTOR_H__

#include <queue>
#include <memory>

#include "acceptor.h"
#include "stream_listener.h"

namespace yuan::net
{

class Connection;

class StreamAcceptor : public Acceptor, public StreamListener
{
public:
    ~StreamAcceptor() override = default;

    void enqueue_pending_connection(std::shared_ptr<Connection> conn)
    {
        pending_connections_.push(std::move(conn));
    }

    std::shared_ptr<Connection> dequeue_pending_connection()
    {
        if (pending_connections_.empty()) return nullptr;
        auto conn = std::move(pending_connections_.front());
        pending_connections_.pop();
        return conn;
    }

    bool has_pending_connections() const
    {
        return !pending_connections_.empty();
    }

private:
    std::queue<std::shared_ptr<Connection>> pending_connections_;
};

} // namespace yuan::net

#endif
