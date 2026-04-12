#ifndef __HTTP_SSE_H__
#define __HTTP_SSE_H__

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::http
{
    class HttpRequest;
    class HttpResponse;
    class HttpSessionContext;

    // SSE 事件数据
    struct SseEvent
    {
        std::string id;        // 可选的事件ID（用于Last-Event-ID）
        std::string event;     // 可选的事件类型名
        std::string data;      // 事件数据
        uint32_t retry = 0;    // 重连间隔(ms)，0表示不设置

        std::string serialize() const;
    };

    // SSE 连接管理 - 管理一个活跃的SSE连接
    class SseConnection
    {
    public:
        SseConnection(HttpRequest *req, HttpResponse *resp);
        ~SseConnection();

        // 禁用拷贝和移动
        SseConnection(const SseConnection&) = delete;
        SseConnection& operator=(const SseConnection&) = delete;

        // 发送事件
        void send(const SseEvent &event);
        void send(const std::string &data, const std::string &event = "", const std::string &id = "");
        
        // 批量发送多条事件（减少syscall）
        void send_batch(const std::vector<SseEvent> &events);

        // 注释心跳 (": heartbeat\n\n" 保持连接活跃)
        void heartbeat();

        // 关闭SSE连接
        void close();

        bool is_active() const { return active_.load(); }

        // 获取客户端IP（用于日志等）
        std::string get_peer_ip() const;

        // 获取连接ID
        uint64_t get_connection_id() const { return conn_id_; }

    private:
        void do_send(const std::string &payload);

        HttpRequest *req_;
        HttpResponse *resp_;
        Connection *conn_;
        uint64_t conn_id_;
        std::atomic<bool> active_{false};
        mutable std::mutex mutex_;
    };

    // SSE 通道 - 管理多个SSE订阅者（广播模式）
    class SseChannel : public std::enable_shared_from_this<SseChannel>
    {
    public:
        explicit SseChannel(const std::string &name, size_t max_clients = 1024);
        ~SseChannel();

        // 订阅此频道
        bool subscribe(SseConnection *conn);

        // 取消订阅
        void unsubscribe(uint64_t conn_id);

        // 广播事件到所有订阅者
        void broadcast(const SseEvent &event);
        void broadcast(const std::string &data, const std::string &event = "");

        // 获取当前活跃连接数
        size_t active_count() const;

        const std::string& name() const { return name_; }

    private:
        std::string name_;
        size_t max_clients_;
        mutable std::mutex mutex_;
        std::unordered_map<uint64_t, SseConnection*> subscribers_;
    };

    // SSE 管理器 - 全局管理所有SseChannel
    class SseManager
    {
    public:
        static SseManager& instance();

        // 创建/获取频道
        std::shared_ptr<SseChannel> get_or_create_channel(const std::string &name, size_t max_clients = 1024);

        // 获取已有频道
        std::shared_ptr<SseChannel> get_channel(const std::string &name) const;

        // 删除频道
        void remove_channel(const std::string &name);

        // 清理所有
        void clear();

    private:
        SseManager() = default;
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<SseChannel>> channels_;
    };
}

#endif
