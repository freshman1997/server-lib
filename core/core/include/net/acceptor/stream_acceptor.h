#ifndef __YUAN_NET_ACCEPTOR_STREAM_ACCEPTOR_H__
#define __YUAN_NET_ACCEPTOR_STREAM_ACCEPTOR_H__

#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <memory>
#include <unordered_map>

#include "acceptor.h"
#include "stream_listener.h"

namespace yuan::net
{

class Connection;

class StreamAcceptor : public Acceptor, public StreamListener
{
public:
    ~StreamAcceptor() override = default;

    using AcceptWaiter = std::function<void(const std::shared_ptr<Connection> &)>;

    uint64_t add_accept_waiter(AcceptWaiter waiter)
    {
        if (!waiter) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto id = next_waiter_id_++;
        accept_waiters_[id] = std::move(waiter);
        return id;
    }

    void remove_accept_waiter(uint64_t id)
    {
        if (id == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        accept_waiters_.erase(id);
    }

    void notify_accept_waiters(const std::shared_ptr<Connection> &conn)
    {
        AcceptWaiter waiter;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (accept_waiters_.empty()) {
                if (conn && queue_pending_connections_) {
                    pending_connections_.push(conn);
                }
                return;
            }
            auto it = accept_waiters_.begin();
            waiter = std::move(it->second);
            accept_waiters_.erase(it);
        }

        if (waiter) {
            waiter(conn);
        }
    }

    void enqueue_pending_connection(std::shared_ptr<Connection> conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_connections_.push(std::move(conn));
    }

    void set_queue_pending_connections(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_pending_connections_ = enabled;
        if (!enabled) {
            std::queue<std::shared_ptr<Connection>> empty;
            pending_connections_.swap(empty);
        }
    }

    std::shared_ptr<Connection> dequeue_pending_connection()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_connections_.empty()) return nullptr;
        auto conn = std::move(pending_connections_.front());
        pending_connections_.pop();
        return conn;
    }

    bool has_pending_connections() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !pending_connections_.empty();
    }

private:
    std::queue<std::shared_ptr<Connection>> pending_connections_;
    bool queue_pending_connections_ = true;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, AcceptWaiter> accept_waiters_;
    uint64_t next_waiter_id_ = 1;
};

} // namespace yuan::net

#endif
