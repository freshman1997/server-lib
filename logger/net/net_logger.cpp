#include "net_logger.h"
#include "base/owner_ptr.h"
#include "base/time.h"
#include "buffer/byte_buffer.h"
#include "formatter.h"
#include "event/event_loop.h"
#include "net/connection/connection.h"
#include "net/connector/tcp_connector.h"
#include "net/handler/connection_handler.h"
#include "net/handler/connector_handler.h"
#include "net/poller/poller.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "timer/timer_handle.h"
#include "timer/wheel_timer_manager.h"

#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace yuan::log
{

static inline LogItem make_log_item(Level level, const std::string& msg, const std::string& name,
                                    const char* file = nullptr, int line = 0, const char* func = nullptr)
{
    LogItem item;
    item.level = level;
    item.message = msg;
    item.logger_name = name;
    item.line = line;

    const auto now_ms = yuan::base::time::system_now_ms();
    item.timestamp = static_cast<std::time_t>(now_ms / 1000ULL);
    item.milliseconds = now_ms % 1000ULL;

    if (file) item.source_file = file;
    if (func) item.function_name = func;
    return item;
}

static std::string vformat_message(const char* fmt, va_list args)
{
    char stack_buf[4096];
    va_list args_copy;
    va_copy(args_copy, args);
    const int written = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
    va_end(args_copy);

    if (written < 0) return {};
    if (static_cast<size_t>(written) < sizeof(stack_buf)) {
        return std::string(stack_buf, static_cast<size_t>(written));
    }

    std::vector<char> heap_buf(static_cast<size_t>(written) + 1, '\0');
    va_list args_retry;
    va_copy(args_retry, args);
    std::vsnprintf(heap_buf.data(), heap_buf.size(), fmt, args_retry);
    va_end(args_retry);
    return std::string(heap_buf.data(), static_cast<size_t>(written));
}

#ifdef _WIN32
static void ensure_winsock_initialized()
{
    static bool initialized = []() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)initialized;
}
#endif

class NetLogger::Impl : public yuan::net::ConnectorHandler, public yuan::net::ConnectionHandler
{
public:
    explicit Impl(const LogConfig& cfg)
        : cfg_(cfg)
    {
#ifdef _WIN32
        ensure_winsock_initialized();
#endif
        start_runtime();
    }

    ~Impl() override
    {
        stop_runtime();
    }

    bool connect();
    void disconnect();
    bool is_connected() const;
    void send(const std::string& data);
    void flush();

    void on_connect_result(const yuan::net::ConnectResult& result) override;

    void on_connected(yuan::net::Connection &conn) override;
    void on_error(yuan::net::Connection &conn) override;
    void on_read(yuan::net::Connection &conn) override;
    void on_write(yuan::net::Connection &conn) override;
    void on_close(yuan::net::Connection &conn) override;

private:
    void start_runtime();
    void stop_runtime();
    bool schedule_connect_locked();
    bool schedule_reconnect_locked();
    void cancel_reconnect_timer_locked();
    void enqueue_pending_locked(std::string data);
    void flush_pending_locked();
    void queue_send_locked(std::string data);
    void write_on_loop(const std::string& data);
    void fallback_to_stderr(const std::string& data) const;

private:
    LogConfig cfg_;
    mutable std::mutex mutex_;
    std::condition_variable state_cv_;
    std::deque<std::string> pending_messages_;
    std::shared_ptr<yuan::net::TcpConnector> connector_;
    std::shared_ptr<yuan::net::Connection> connection_;
    yuan::net::Poller* poller_ = nullptr;
    yuan::timer::WheelTimerManager* timer_manager_ = nullptr;
    yuan::net::EventLoop* loop_ = nullptr;
    std::thread loop_thread_;
    bool runtime_started_ = false;
    bool connecting_ = false;
    bool connected_ = false;
    bool shutting_down_ = false;
    yuan::timer::TimerHandle reconnect_timer_;
    int reconnect_attempts_ = 0;
};

void NetLogger::Impl::start_runtime()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (runtime_started_) return;

    poller_ = yuan::net::NetworkRuntime::create_default_poller();
    timer_manager_ = new yuan::timer::WheelTimerManager();
    if (!poller_ || !poller_->init()) {
        delete poller_;
        poller_ = nullptr;
        delete timer_manager_;
        timer_manager_ = nullptr;
        return;
    }

    loop_ = new yuan::net::EventLoop(poller_, timer_manager_);
    runtime_started_ = true;
    loop_thread_ = std::thread([this]() {
        if (loop_) {
            loop_->loop();
        }
    });
}

void NetLogger::Impl::stop_runtime()
{
    std::thread loop_thread;
    yuan::net::EventLoop* loop = nullptr;
    yuan::net::Poller* poller = nullptr;
    yuan::timer::WheelTimerManager* timer_manager = nullptr;
    std::shared_ptr<yuan::net::TcpConnector> connector;
    std::shared_ptr<yuan::net::Connection> connection;
    bool was_connecting = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!runtime_started_) return;

        shutting_down_ = true;
        was_connecting = connecting_;
        connected_ = false;
        connecting_ = false;
        pending_messages_.clear();
        cancel_reconnect_timer_locked();
        connector = connector_;
        connection = connection_;
        connector_.reset();
        connection_ = nullptr;

        loop = loop_;
        poller = poller_;
        timer_manager = timer_manager_;
        loop_ = nullptr;
        poller_ = nullptr;
        timer_manager_ = nullptr;
        runtime_started_ = false;
        loop_thread = std::move(loop_thread_);
    }

    if (loop) {
        loop->queue_in_loop([connector, connection, was_connecting, loop]() {
            if (was_connecting && connector) connector->cancel();
            if (connection) connection->abort();
            loop->quit();
        });
    }
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
    connector.reset();
    delete loop;
    delete poller;
    delete timer_manager;
}

bool NetLogger::Impl::schedule_connect_locked()
{
    if (!runtime_started_ || !loop_ || shutting_down_) return false;
    if (connected_ || connecting_) return true;

    connecting_ = true;
    if (!connector_) {
        connector_ = std::make_shared<yuan::net::TcpConnector>();
        connector_->set_data(timer_manager_,
                             std::shared_ptr<yuan::net::ConnectorHandler>(static_cast<yuan::net::ConnectorHandler *>(this),
                                                                           [](yuan::net::ConnectorHandler *) {}),
                             loop_);
    }

    const auto address = yuan::net::InetAddress(cfg_.net_server_ip, cfg_.net_server_port);
    loop_->queue_in_loop([this, address]() {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connector_ || !loop_ || shutting_down_) return;
            ok = connector_->connect(address, cfg_.net_connect_timeout_ms, 0);
            if (!ok) {
                connecting_ = false;
                state_cv_.notify_all();
            }
        }
        if (!ok) {
            fallback_to_stderr("[NetLogger-Fallback] connect failed");
        }
    });
    return true;
}

    void NetLogger::Impl::cancel_reconnect_timer_locked()
{
    reconnect_timer_.cancel();
    reconnect_timer_.reset();
}

bool NetLogger::Impl::schedule_reconnect_locked()
{
    if (!cfg_.net_auto_reconnect) return false;
    if (!runtime_started_ || !loop_ || !timer_manager_ || shutting_down_) return false;
    if (connected_ || connecting_ || pending_messages_.empty()) return false;
    if (reconnect_timer_) return true;
    if (cfg_.net_max_retries >= 0 && reconnect_attempts_ >= cfg_.net_max_retries) {
        while (!pending_messages_.empty()) {
            fallback_to_stderr(pending_messages_.front());
            pending_messages_.pop_front();
        }
        return false;
    }

    reconnect_timer_ = timer_manager_->after(
        static_cast<uint32_t>(cfg_.net_reconnect_delay_ms),
        [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            reconnect_timer_.reset();
            ++reconnect_attempts_;
            schedule_connect_locked();
        });
    return static_cast<bool>(reconnect_timer_);
}

void NetLogger::Impl::enqueue_pending_locked(std::string data)
{
    const int max_pending = cfg_.net_max_pending_messages;
    if (max_pending > 0 && static_cast<int>(pending_messages_.size()) >= max_pending) {
        if (cfg_.net_drop_oldest_on_overflow) {
            fallback_to_stderr("[NetLogger-Fallback] pending queue full, dropping oldest message");
            pending_messages_.pop_front();
        } else {
            fallback_to_stderr("[NetLogger-Fallback] pending queue full, dropping newest message");
            return;
        }
    }
    pending_messages_.push_back(std::move(data));
}

bool NetLogger::Impl::connect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return schedule_connect_locked();
}

void NetLogger::Impl::disconnect()
{
    std::shared_ptr<yuan::net::TcpConnector> connector;
    std::shared_ptr<yuan::net::Connection> connection;
    yuan::net::EventLoop* loop = nullptr;
    bool was_connecting = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_connecting = connecting_;
        connected_ = false;
        connecting_ = false;
        cancel_reconnect_timer_locked();
        connector = connector_;
        connection = connection_;
        loop = loop_;
        connection_ = nullptr;
        state_cv_.notify_all();
    }

    if (loop) {
        loop->queue_in_loop([connector, connection, was_connecting]() {
            if (was_connecting && connector) connector->cancel();
            if (connection) connection->abort();
        });
    }
}

bool NetLogger::Impl::is_connected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

void NetLogger::Impl::queue_send_locked(std::string data)
{
    if (!runtime_started_ || !loop_ || shutting_down_) {
        fallback_to_stderr(data);
        return;
    }

    loop_->queue_in_loop([this, data = std::move(data)]() {
        write_on_loop(data);
    });
}

void NetLogger::Impl::write_on_loop(const std::string& data)
{
    std::shared_ptr<yuan::net::Connection> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || !connection_) {
            enqueue_pending_locked(data);
            if (!schedule_connect_locked()) {
                schedule_reconnect_locked();
            }
            return;
        }
        connection = connection_;
    }

    yuan::buffer::ByteBuffer buffer(data.size() + 1);
    buffer.append(std::string_view(data));
    buffer.append("\n", 1);
    connection->write_and_flush(buffer);
}

void NetLogger::Impl::send(const std::string& data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_ && connection_) {
        queue_send_locked(data);
        return;
    }

    enqueue_pending_locked(data);
    if (!schedule_connect_locked() && !schedule_reconnect_locked()) {
        if (!pending_messages_.empty()) {
            auto pending = std::move(pending_messages_.back());
            pending_messages_.pop_back();
            fallback_to_stderr(pending);
        }
    }
}

void NetLogger::Impl::flush_pending_locked()
{
    if (!loop_ || !connected_ || !connection_) return;

    while (!pending_messages_.empty()) {
        auto data = std::move(pending_messages_.front());
        pending_messages_.pop_front();
        loop_->queue_in_loop([this, data = std::move(data)]() {
            write_on_loop(data);
        });
    }
}

void NetLogger::Impl::flush()
{
    std::shared_ptr<yuan::net::Connection> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            flush_pending_locked();
            connection = connection_;
        }
    }

    if (loop_ && connection) {
        loop_->queue_in_loop([connection]() {
            connection->flush();
        });
    }
    std::cerr.flush();
}

void NetLogger::Impl::fallback_to_stderr(const std::string& data) const
{
    std::cerr << data << '\n';
}

void NetLogger::Impl::on_connect_result(const yuan::net::ConnectResult& result)
{
    const auto& conn = result.connection;
    std::lock_guard<std::mutex> lock(mutex_);

    if (result.code == yuan::net::ConnectResultCode::success) {
        cancel_reconnect_timer_locked();
        reconnect_attempts_ = 0;
        connection_ = conn;
        connected_ = true;
        connecting_ = false;
        if (conn) {
            conn->set_connection_handler(yuan::net::make_non_owning_handler(this));
        }
        flush_pending_locked();
        state_cv_.notify_all();
        return;
    }

    if (result.code == yuan::net::ConnectResultCode::timeout) {
        fallback_to_stderr("[NetLogger-Fallback] connect timeout, attempt=" + std::to_string(result.attempt_id));
    } else {
        fallback_to_stderr("[NetLogger-Fallback] connect failed, err=" + std::to_string(result.error_code)
            + ", attempt=" + std::to_string(result.attempt_id));
    }

    connecting_ = false;
    connected_ = false;
    if (connection_ == conn) {
        connection_ = nullptr;
    }
    schedule_reconnect_locked();
    state_cv_.notify_all();
}

void NetLogger::Impl::on_connected(yuan::net::Connection &conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_reconnect_timer_locked();
    reconnect_attempts_ = 0;
    connection_ = conn.shared_from_this();
    connected_ = true;
    connecting_ = false;
    flush_pending_locked();
    state_cv_.notify_all();
}

void NetLogger::Impl::on_error(yuan::net::Connection &conn)
{
    auto conn_ptr = conn.shared_from_this();
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    connecting_ = false;
    if (connection_ == conn_ptr) {
        connection_ = nullptr;
    }
    schedule_reconnect_locked();
    state_cv_.notify_all();
}

void NetLogger::Impl::on_read(yuan::net::Connection &conn)
{
    (void)conn;
}

void NetLogger::Impl::on_write(yuan::net::Connection &conn)
{
    (void)conn;
}

void NetLogger::Impl::on_close(yuan::net::Connection &conn)
{
    auto conn_ptr = conn.shared_from_this();
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    connecting_ = false;
    if (connection_ == conn_ptr) {
        connection_ = nullptr;
    }
    schedule_reconnect_locked();
    state_cv_.notify_all();
}

NetLogger::NetLogger(const LogConfig& cfg)
    : config_(cfg), connection_impl_(std::make_unique<Impl>(cfg))
{
    set_level(cfg.log_level);
    formatter_ = std::make_shared<Formatter>(cfg.fmt_pattern, cfg.fmt_datefmt);
}

NetLogger::NetLogger(const std::string& server_ip, int server_port)
{
    config_.net_server_ip = server_ip;
    config_.net_server_port = server_port;
    connection_impl_ = std::make_unique<Impl>(config_);
    set_level(config_.log_level);
    formatter_ = std::make_shared<Formatter>();
}

NetLogger::~NetLogger()
{
    flush();
    connection_impl_.reset();
}

void NetLogger::log(Level level, const char* fmt, ...)
{
    if (level < level_.load(std::memory_order_relaxed)) return;

    va_list args;
    va_start(args, fmt);
    const std::string msg = vformat_message(fmt, args);
    va_end(args);

    log_impl(level, msg);
}

void NetLogger::log_impl(Level level, const std::string& msg)
{
    LogItem item = make_log_item(level, msg, name_);
    std::string formatted;
    try {
        formatted = format_log_item(item);
    } catch (...) {
        formatted = "[format_error] " + msg;
    }

    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto* impl = yuan::base::owner_ptr(connection_impl_);
    if (impl) impl->send(formatted);
}

void NetLogger::log_impl(Level level, const std::string& msg,
                         const char* file, int line, const char* func)
{
    LogItem item = make_log_item(level, msg, name_, file, line, func);
    std::string formatted;
    try {
        formatted = format_log_item(item);
    } catch (...) {
        formatted = "[format_error] " + msg;
    }

    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto* impl = yuan::base::owner_ptr(connection_impl_);
    if (impl) impl->send(formatted);
}

void NetLogger::flush()
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto* impl = yuan::base::owner_ptr(connection_impl_);
    if (impl) impl->flush();
}

bool NetLogger::connect()
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto* impl = yuan::base::owner_ptr(connection_impl_);
    return impl ? impl->connect() : false;
}

void NetLogger::disconnect()
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto* impl = yuan::base::owner_ptr(connection_impl_);
    if (impl) impl->disconnect();
}

bool NetLogger::is_connected() const
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    const auto* impl = yuan::base::owner_ptr(connection_impl_);
    return impl ? impl->is_connected() : false;
}

} // namespace yuan::log
