#include "sse.h"
#include "context.h"
#include "request.h"
#include "response.h"
#include "net/socket/inet_address.h"
#include "net/connection/connection.h"

#include <atomic>
#include <sstream>

namespace yuan::net::http
{
    std::shared_ptr<SseConnection> SseConnection::create(HttpRequest *req, HttpResponse *resp)
    {
        return std::make_shared<SseConnection>(req, resp);
    }

    std::string SseEvent::serialize() const
    {
        std::ostringstream oss;
        if (!id.empty()) {
            oss << "id: " << id << "\n";
        }
        if (!event.empty()) {
            oss << "event: " << event << "\n";
        }
        // data字段支持多行（每行以data:开头）
        // 简单处理：按\n分割
        size_t pos = 0;
        while (pos < data.size()) {
            size_t end = data.find('\n', pos);
            if (end == std::string::npos) {
                oss << "data: " << data.substr(pos) << "\n";
                break;
            }
            oss << "data: " << data.substr(pos, end - pos) << "\n";
            pos = end + 1;
        }
        if (retry > 0) {
            oss << "retry: " << retry << "\n";
        }
        oss << "\n";
        return oss.str();
    }

    SseConnection::SseConnection(HttpRequest * req, HttpResponse * resp)
        : req_(req), resp_(resp)
    {
        if (!req_ || !resp_) {
            return;
        }

        auto *ctx = req_->get_context();
        if (!ctx) {
            return;
        }
        if (auto owner = ctx->connection()) {
            conn_owner_ = owner;
        }
        auto conn = conn_owner_.lock();
        if (!conn) {
            return;
        }
        conn_id_ = reinterpret_cast<uint64_t>(conn.get());
        active_.store(true);

        // Configure SSE response headers.
        resp_->set_sse();
        resp_->set_response_code(ResponseCode::ok_);
        resp_->add_header("Content-Type", "text/event-stream");
        resp_->add_header("Cache-Control", "no-cache, no-transform");
        resp_->add_header("Connection", "keep-alive");
        resp_->add_header("X-Accel-Buffering", "no");
        resp_->add_header("Access-Control-Allow-Origin", "*");

        resp_->pack_and_send(conn.get());
    }

    SseConnection::~SseConnection()
    {
        close();
    }

    void SseConnection::send(const SseEvent & event)
    {
        do_send(event.serialize());
    }

    void SseConnection::send(const std::string & data, const std::string & event, const std::string & id)
    {
        SseEvent e;
        e.data = data;
        e.event = event;
        e.id = id;
        send(e);
    }

    void SseConnection::send_batch(const std::vector<SseEvent> & events)
    {
        std::string payload;
        payload.reserve(4096);
        for (const auto &e : events) {
            payload.append(e.serialize());
        }
        do_send(payload);
    }

    void SseConnection::heartbeat()
    {
        do_send(": heartbeat\r\n\r\n"); // SSE注释行，保持连接活跃
    }

    void SseConnection::close()
    {
        bool expected = true;
        if (active_.compare_exchange_strong(expected, false)) {
            // 可以在这里通知channel取消订阅
        }
    }

    std::string SseConnection::get_peer_ip() const
    {
        auto conn = conn_owner_.lock();
        if (!conn)
            return {};
    }

    void SseConnection::do_send(const std::string & payload)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto conn = conn_owner_.lock();
        if (!active_.load() || !conn || !conn->is_connected()) {
            active_.store(false);
            return;
        }

        conn->append_output(payload);
        conn->flush();
    }

    SseChannel::SseChannel(const std::string & name, size_t max_clients)
        : name_(name), max_clients_(max_clients)
    {
    }

    SseChannel::~SseChannel()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto & [
                        id,
                        conn
                    ] : subscribers_) {
            (void)id;
            if (auto owner = conn.lock()) {
                owner->close();
            }
        }
        subscribers_.clear();
    }

    bool SseChannel::subscribe(const std::shared_ptr<SseConnection> & conn)
    {
        if (!conn)
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        if (subscribers_.size() >= max_clients_) {
            return false; // 已满
        }

        subscribers_[conn->get_connection_id()] = conn;
        return true;
    }

    bool SseChannel::subscribe(SseConnection * conn)
    {
        if (!conn) {
            return false;
        }
        try {
            return subscribe(conn->shared_from_this());
        } catch (const std::bad_weak_ptr &) {
            return false;
        }
    }

    void SseChannel::unsubscribe(uint64_t conn_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_.erase(conn_id);
    }

    void SseChannel::broadcast(const SseEvent & event)
    {
        std::vector<std::shared_ptr<SseConnection>> active;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = subscribers_.begin(); it != subscribers_.end();) {
                auto conn = it->second.lock();
                if (conn && conn->is_active()) {
                    active.push_back(std::move(conn));
                    ++it;
                } else {
                    it = subscribers_.erase(it);
                }
            }
        }

        for (const auto &conn : active) {
            conn->send(event);
        }
    }

    void SseChannel::broadcast(const std::string & data, const std::string & event)
    {
        broadcast(SseEvent{ "", event, data });
    }

    void SseChannel::heartbeat()
    {
        std::vector<std::shared_ptr<SseConnection>> active;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = subscribers_.begin(); it != subscribers_.end();) {
                auto conn = it->second.lock();
                if (conn && conn->is_active()) {
                    active.push_back(std::move(conn));
                    ++it;
                } else {
                    it = subscribers_.erase(it);
                }
            }
        }

        for (const auto &conn : active) {
            conn->heartbeat();
        }
    }

    size_t SseChannel::active_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto &[id, conn] : subscribers_) {
            (void)id;
            auto owner = conn.lock();
            if (owner && owner->is_active()) {
                ++count;
            }
        }
        return count;
    }

    SseManager &SseManager::instance()
    {
        static SseManager inst;
        return inst;
    }

    std::shared_ptr<SseChannel> SseManager::get_or_create_channel(const std::string & name, size_t max_clients)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(name);
        if (it != channels_.end()) {
            return it->second;
        }
        auto ch = std::make_shared<SseChannel>(name, max_clients);
        channels_[name] = ch;
        return ch;
    }

    std::shared_ptr<SseChannel> SseManager::get_channel(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = channels_.find(name);
        return (it != channels_.end()) ? it->second : nullptr;
    }

    void SseManager::remove_channel(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.erase(name);
    }

    void SseManager::clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_.clear();
    }
}
