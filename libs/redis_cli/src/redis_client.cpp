#include "redis_client.h"
#include "coroutine/sync_wait.h"
#include "event/event_loop.h"
#include "internal/cmd_builder.h"
#include "internal/coroutine.h"
#include "internal/redis_registry.h"
#include "net/connection/connection_factory.h"
#include "net/connection/stream_transport.h"
#include "net/socket/socket.h"
#include "net/socket/inet_address.h"
#include "redis_value.h"
#include "internal/redis_impl.h"
#include "value/error_value.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

namespace yuan::redis
{
    PipelineCommand::PipelineCommand(std::string command_name, std::vector<std::string> command_args)
        : name(std::move(command_name))
        , args(std::move(command_args))
    {
    }

    PipelineCommand::PipelineCommand(std::string command_name, std::initializer_list<std::string_view> command_args)
        : name(std::move(command_name))
    {
        args.reserve(command_args.size());
        for (const auto arg : command_args) {
            args.emplace_back(arg);
        }
    }

    PipelineCommand PipelineCommand::get(std::string key)
    {
        return {"get", {std::move(key)}};
    }

    PipelineCommand PipelineCommand::set(std::string key, std::string value)
    {
        return {"set", {std::move(key), std::move(value)}};
    }

    PipelineCommand PipelineCommand::del(std::vector<std::string> keys)
    {
        return {"del", std::move(keys)};
    }

    PipelineCommand PipelineCommand::incr(std::string key)
    {
        return {"incr", {std::move(key)}};
    }

    PipelineCommand PipelineCommand::expire(std::string key, int seconds)
    {
        return {"expire", {std::move(key), std::to_string(seconds)}};
    }

    PipelineCommand PipelineCommand::hset(std::string key, std::string field, std::string value)
    {
        return {"hset", {std::move(key), std::move(field), std::move(value)}};
    }

    PipelineCommand PipelineCommand::hget(std::string key, std::string field)
    {
        return {"hget", {std::move(key), std::move(field)}};
    }

    PipelineCommand PipelineCommand::lpush(std::string key, std::vector<std::string> values)
    {
        values.insert(values.begin(), std::move(key));
        return {"lpush", std::move(values)};
    }

    PipelineCommand PipelineCommand::rpush(std::string key, std::vector<std::string> values)
    {
        values.insert(values.begin(), std::move(key));
        return {"rpush", std::move(values)};
    }

    RedisClient::RedisClient()
    {
        impl_ = std::make_shared<Impl>();
        impl_->client_ = this;
        impl_->registry_ = RedisRegistry::get_instance();
    }

    RedisClient::RedisClient(const Option & opt)
    {
        impl_ = std::make_shared<Impl>();
        impl_->option_ = opt;
        impl_->client_ = this;
        impl_->registry_ = RedisRegistry::get_instance();
    }

    RedisClient::RedisClient(const Option &opt, std::shared_ptr<void> registry)
    {
        impl_ = std::make_shared<Impl>();
        impl_->option_ = opt;
        impl_->client_ = this;
        impl_->registry_ = std::static_pointer_cast<RedisRegistry>(std::move(registry));
        if (!impl_->registry_) {
            impl_->registry_ = RedisRegistry::get_instance();
        }
    }

    RedisClient::~RedisClient()
    {
        close();
    }

    int RedisClient::connect()
    {
        using namespace yuan::net;

        if (is_connected()) {
            return 0;
        }

        const InetAddress addr{ impl_->option_.host_, impl_->option_.port_ };
        if (addr.get_port() <= 0 || addr.get_port() > 65535) {
            impl_->last_error_.store(ErrorValue::from_string("port is invalid"));
            return -1;
        }

        auto sock = std::make_unique<net::Socket>(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            impl_->last_error_.store(ErrorValue::from_string("create socket failed"));
            return -1;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            const int socket_error = sock->last_error();
            impl_->last_error_.store(ErrorValue::from_string(
                impl_->option_.name_ + " connect failed, socket error: " + std::to_string(socket_error)));
            return -1;
        }

        const auto loop = impl_->registry()->get_event_loop();

        auto conn = create_stream_connection(sock.release());
        conn->set_connection_handler(impl_);
        conn->set_event_handler(loop);
        if (auto stream = std::dynamic_pointer_cast<net::StreamTransport>(conn)) {
            auto *channel = stream->stream_channel();
            if (!channel) {
                conn->close();
                impl_->last_error_.store(ErrorValue::from_string("stream channel is invalid"));
                return -1;
            }
        } else {
            conn->close();
            impl_->last_error_.store(ErrorValue::from_string("connection is not a stream transport"));
            return -1;
        }

        loop->on_new_connection(conn);

        impl_->on_do_connect(conn);
        impl_->completion_event_.reset(loop);

        const auto runtime = impl_->registry()->get_coroutine_runtime();
        auto wait_connect = [this]()->SimpleTask<bool>
        {
            const auto timer_manager = impl_->registry()->get_timer_manager();
            const int ct = impl_->option_.connect_timeout_ms_ > 0 ? impl_->option_.connect_timeout_ms_
                : (impl_->option_.timeout_ms_ > 0 ? impl_->option_.timeout_ms_ : 0);
            const bool timed_out = co_await impl_->completion_event_.wait_for(
                timer_manager,
                ct > 0 ? static_cast<uint32_t>(ct) : 0);
            co_return !timed_out && is_connected();
        };
        bool res = false;
        try {
            res = yuan::coroutine::sync_wait_locked(
                runtime,
                wait_connect(),
                "redis event loop is already running on another thread");
        } catch (const std::exception &ex) {
            impl_->last_error_.store(ErrorValue::from_string(ex.what()));
            impl_->disconnect();
            return -1;
        }

        if (!res) {
            const int ct = impl_->option_.connect_timeout_ms_ > 0 ? impl_->option_.connect_timeout_ms_
                : (impl_->option_.timeout_ms_ > 0 ? impl_->option_.timeout_ms_ : 0);
            impl_->last_error_.store(ErrorValue::from_string(impl_->option_.name_ + " connect timeout" +
                (ct > 0 ? " (" + std::to_string(ct) + "ms)" : "")));
            impl_->disconnect();
            return -1;
        }

        impl_->clear_mask(RedisState::connecting);

        if (!impl_->option_.username_.empty() || !impl_->option_.password_.empty()) {
            const auto auth_result = impl_->option_.username_.empty()
                ? auth(impl_->option_.password_)
                : auth(impl_->option_.username_, impl_->option_.password_);
            if (!auth_result) {
                impl_->last_error_.store(ErrorValue::from_string("auth failed"));
                impl_->disconnect();
                return -1;
            }
        }

        if (impl_->option_.db_ != 0) {
            if (const auto select_result = select(impl_->option_.db_); !select_result) {
                impl_->last_error_.store(ErrorValue::from_string("select db failed"));
                impl_->disconnect();
                return -1;
            }
        }

        return 0;
    }

    void RedisClient::set_option(const Option & opt)
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);
        impl_->option_ = opt;
    }

    bool RedisClient::is_connected() const
    {
        return impl_->is_connected();
    }

    bool RedisClient::is_closed() const
    {
        return impl_->is_closed();
    }

    bool RedisClient::is_timeout() const
    {
        return impl_->is_timeout();
    }

    void RedisClient::close()
    {
        if (!impl_) {
            return;
        }

        impl_->close();
    }

    void RedisClient::disconnect()
    {
        if (!impl_) {
            return;
        }

        impl_->disconnect();
    }

    std::shared_ptr<RedisValue> RedisClient::get_last_error() const
    {
        return impl_->last_error_.load();
    }

    void RedisClient::set_last_error(std::shared_ptr<RedisValue> error)
    {
        impl_->last_error_.store(error);
    }

    const std::string &RedisClient::get_name() const
    {
        return impl_->option_.name_;
    }

    void RedisClient::unsubscribe_channel(const std::string & channel)
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);

        if (impl_->subcribe_cmd) {
            impl_->subcribe_cmd->unsubcribe(channel);
            if (!impl_->subcribe_cmd->is_subcribe()) {
                impl_->subcribe_cmd = nullptr;
            }
        }
    }

    int RedisClient::receive(int timeout)
    {
        return impl_->fetch_next_message(timeout);
    }

    int RedisClient::receive()
    {
        return impl_->fetch_next_message(0);
    }

    bool RedisClient::is_subscribing() const
    {
        return impl_->subcribe_cmd != nullptr && impl_->subcribe_cmd->is_subcribe();
    }

    bool RedisClient::ensure_connected()
    {
        if (impl_->is_connected()) {
            return true;
        }

        if (impl_->closed_by_user_.load(std::memory_order_acquire)) {
            return false;
        }

        if (!impl_->option_.reconnect_) {
            return false;
        }

        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);

        if (impl_->is_connected()) {
            return true;
        }

        if (impl_->closed_by_user_.load(std::memory_order_acquire)) {
            return false;
        }

        impl_->closed_by_user_.store(false, std::memory_order_release);

        for (int i = 0; i < impl_->option_.max_reconnect_retries_; ++i) {
            impl_->reconnect_attempts_.fetch_add(1, std::memory_order_release);
            if (i > 0 && impl_->option_.reconnect_delay_ms_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(impl_->option_.reconnect_delay_ms_));
            }
            if (connect() == 0) {
                impl_->reconnect_successes_.fetch_add(1, std::memory_order_release);
                return true;
            }
        }

        impl_->last_error_.store(ErrorValue::from_string(impl_->option_.name_ + " reconnect failed in ensure_connected"));
        return false;
    }

    bool RedisClient::wait_in_flight(uint32_t timeout_ms)
    {
        std::unique_lock<std::mutex> lock(impl_->in_flight_mutex_);
        if (timeout_ms == 0)
        {
            return impl_->in_flight_.load(std::memory_order_acquire) == 0;
        }
        return impl_->in_flight_cv_.wait_for(lock,
            std::chrono::milliseconds(timeout_ms),
            [this]() { return impl_->in_flight_.load(std::memory_order_acquire) == 0; });
    }

    RedisClientStats RedisClient::stats() const
    {
        RedisClientStats out;
        out.connected = impl_->is_connected();
        out.closed = impl_->is_closed();
        out.timeout = impl_->is_timeout();
        out.in_flight = impl_->in_flight_.load(std::memory_order_acquire);
        out.reconnect_attempts = impl_->reconnect_attempts_.load(std::memory_order_acquire);
        out.reconnect_successes = impl_->reconnect_successes_.load(std::memory_order_acquire);
        out.command_timeouts = impl_->command_timeouts_.load(std::memory_order_acquire);
        out.protocol_errors = impl_->protocol_errors_.load(std::memory_order_acquire);
        out.commands_total = impl_->commands_total_.load(std::memory_order_acquire);
        out.command_errors = impl_->command_errors_.load(std::memory_order_acquire);
        out.total_latency_us = impl_->total_latency_us_.load(std::memory_order_acquire);
        out.health_checks = impl_->health_checks_.load(std::memory_order_acquire);
        out.health_check_successes = impl_->health_check_successes_.load(std::memory_order_acquire);
        out.health_check_failures = impl_->health_check_failures_.load(std::memory_order_acquire);
        return out;
    }

    HealthCheckResult RedisClient::try_ping()
    {
        if (!impl_->operation_mutex_.try_lock()) {
            return HealthCheckResult::busy;
        }
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_, std::adopt_lock);

        if (impl_->in_flight_.load(std::memory_order_acquire) > 0) {
            return HealthCheckResult::busy;
        }

        if (!is_connected()) {
            return HealthCheckResult::disconnected;
        }

        const int hc_timeout = impl_->option_.health_check_timeout_ms_ > 0
            ? impl_->option_.health_check_timeout_ms_ : 2000;
        auto result = impl_->execute_command(make_cmd("ping"), hc_timeout);

        if (!result) {
            return HealthCheckResult::failed;
        }

        return HealthCheckResult::ok;
    }

    void RedisClient::start_health_check()
    {
        if (impl_->option_.health_check_interval_ms_ <= 0) {
            return;
        }

        impl_->health_check_closing_.store(false, std::memory_order_release);
        impl_->health_checks_.store(0, std::memory_order_release);
        impl_->health_check_successes_.store(0, std::memory_order_release);
        impl_->health_check_failures_.store(0, std::memory_order_release);

        auto client_ptr = shared_from_this();
        impl_->health_check_thread_ = std::thread([this, client_ptr]() {
            while (!impl_->health_check_closing_.load(std::memory_order_acquire)) {
                {
                    std::unique_lock<std::mutex> lock(impl_->health_check_mutex_);
                    if (impl_->health_check_cv_.wait_for(lock,
                        std::chrono::milliseconds(impl_->option_.health_check_interval_ms_),
                        [this]() { return impl_->health_check_closing_.load(std::memory_order_acquire); })) {
                        break;
                    }
                }

                if (impl_->health_check_closing_.load(std::memory_order_acquire)) {
                    break;
                }

                impl_->health_checks_.fetch_add(1, std::memory_order_release);

                if (!is_connected()) {
                    if (ensure_connected()) {
                        impl_->health_check_successes_.fetch_add(1, std::memory_order_release);
                    } else {
                        impl_->health_check_failures_.fetch_add(1, std::memory_order_release);
                    }
                    continue;
                }

                auto result = try_ping();
                switch (result) {
                case HealthCheckResult::ok:
                    impl_->health_check_successes_.fetch_add(1, std::memory_order_release);
                    break;
                case HealthCheckResult::busy:
                    impl_->health_check_successes_.fetch_add(1, std::memory_order_release);
                    break;
                case HealthCheckResult::disconnected:
                    if (ensure_connected()) {
                        impl_->health_check_successes_.fetch_add(1, std::memory_order_release);
                    } else {
                        impl_->health_check_failures_.fetch_add(1, std::memory_order_release);
                    }
                    break;
                case HealthCheckResult::failed:
                    impl_->health_check_failures_.fetch_add(1, std::memory_order_release);
                    break;
                }
            }
        });
    }

    void RedisClient::stop_health_check()
    {
        impl_->health_check_closing_.store(true, std::memory_order_release);
        impl_->health_check_cv_.notify_all();
        if (impl_->health_check_thread_.joinable()) {
            impl_->health_check_thread_.join();
        }
    }
}
