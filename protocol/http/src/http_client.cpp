#include "logger.h"
#include "coroutine/connection_event_awaitable.h"
#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/connection/stream_transport.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/select_poller.h"
#include "context.h"
#include "http_client.h"
#include "net/socket/socket.h"
#include "ops/option.h"
#include "response.h"
#include "session.h"
#include "timer/timer_util.hpp"
#include "timer/wheel_timer_manager.h"
#include "net/secuity/openssl.h"

namespace yuan::net::http 
{
    HttpClient::RequestState::~RequestState()
    {
        if (session) {
            delete session;
        }

        if (conn_timer) {
            conn_timer->cancel();
        }
    }

    HttpClient::HttpClient()
    {
        port_ = 80;
        host_name_.clear();
        ssl_module_ = nullptr;
        coroutine_request_mode_ = false;
    }

    HttpClient::~HttpClient()
    {
        if (auto *state = request_state()) {
            if (state->event_loop) {
                state->event_loop->quit();
            }
        }
    }

    void HttpClient::on_connected(Connection *conn)
    {
        auto *state = request_state();
        if (!state) {
            return;
        }

        HttpSessionContext *ctx = new HttpSessionContext(conn);
        ctx->set_mode(Mode::client);
        state->session = new HttpSession((uint64_t)conn, ctx, state->timer_manager);
        if (state->ccb) {
            if (state->conn_timer) {
                state->conn_timer->cancel();
                state->conn_timer = nullptr;
            }

            state->ccb(ctx->get_request());
        }
    }

    void HttpClient::on_error(Connection *conn)
    {
        if (auto *state = request_state()) {
            if (state->session && state->session->get_context()) {
                state->session->get_context()->set_connection(nullptr);
            }
        }
        fail_or_complete_request(true);
    }

    void HttpClient::on_read(Connection *conn)
    {
        auto *state = request_state();
        if (!state || !state->session || !state->session->get_context()) {
            return;
        }

        HttpSessionContext *context = state->session->get_context();
        if (!context->parse()) {
            if (context->has_error()) {
                fail_or_complete_request(true);
                return;
            }
            return;
        }

        if (context->has_error()) {
            fail_or_complete_request(true);
            return;
        }

        if (context->is_downloading()) {
            return;
        }

        if (context->is_completed()) {
            (void)context->try_parse_request_content();

            if (!context->is_completed()) {
                return;
            }

            state->request_completed = true;
            if (state->rcb) {
                state->rcb(context->get_request(), context->get_response());
            } else if (coroutine_request_mode_) {
                state->completion_event.notify();
            }
        }
        state->session->reset_timer();
    }

    void HttpClient::on_write(Connection *conn)
    {
        
    }

    void HttpClient::on_close(Connection *conn)
    {
        if (auto *state = request_state()) {
            if (state->session && state->session->get_context()) {
                state->session->get_context()->set_connection(nullptr);
            }
        }
        fail_or_complete_request(true);
    }

    bool HttpClient::query(const std::string &url)
    {
        if (url.find("://") == std::string::npos) {
            return false;
        }
        size_t pos = url.find("://");
        std::string protocol = url.substr(0, pos);
        if (protocol != "http" && protocol != "https") {
            return false;
        }

        std::string rest = url.substr(pos + 3);
        size_t port_pos = rest.find(":");
        if (port_pos != std::string::npos) {
            std::string port_str = rest.substr(port_pos + 1);
            port_ = std::stoi(port_str);
            if (port_ <= 0 || port_ > 65535) {
                return false;
            }
        }

        size_t path_pos = rest.find("/");
        if (path_pos != std::string::npos) {
            std::string path = rest.substr(path_pos);
            if (path.empty()) {
                return false;
            }
        } else {
            if (port_pos != std::string::npos) {
                path_pos = port_pos;
            } else {
                path_pos = rest.size();
            }
        }

        host_name_ = rest.substr(0, port_pos > 0 ? port_pos : path_pos);

        return true;
    }

    Connection *HttpClient::create_connection()
    {
        InetAddress addr{ host_name_.c_str(), port_ };
        if (addr.get_ip().empty()) {
            LOG_WARN("get ip failed!");
            return nullptr;
        }

        if (addr.get_port() <= 0 || addr.get_port() > 65535) {
            LOG_WARN("port is invalid!");
            return nullptr;
        }
        
        net::Socket *sock = new net::Socket(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            LOG_ERROR("create socket fail!");
            delete sock;
            return nullptr;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            LOG_WARN("connect failed");
            delete sock;
            return nullptr;
        }

        config::load_config();

        Connection *conn = create_stream_connection(sock);

    #ifdef HTTP_USE_SSL
        ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!ssl_module_->init("ca-cert.pem", "ca-key.pem", SSLHandler::SSLMode::connector_)) {
            if (auto msg = ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            conn->abort();
            return nullptr;
        }

        conn->set_ssl_handler(ssl_module_->create_handler(sock->get_fd(), SSLHandler::SSLMode::connector_));
    #endif

        return conn;
    }

    void HttpClient::bind_runtime(
        Connection *conn,
        net::EventLoop *event_loop,
        timer::TimerManager *timer_manager,
        connected_callback ccb,
        request_function rcb)
    {
        auto *state = request_state();
        if (!state) {
            return;
        }

        conn->set_connection_handler(this);
        conn->set_event_handler(event_loop);
        if (auto *stream = dynamic_cast<StreamTransport *>(conn)) {
            auto *channel = stream->stream_channel();
            if (channel) {
                event_loop->update_channel(channel);
            }
        }

        state->rcb = std::move(rcb);
        state->ccb = std::move(ccb);
        state->event_loop = event_loop;
        state->timer_manager = timer_manager;
        state->completion_event.bind_loop(event_loop);
    }

    void HttpClient::start_request_state()
    {
        request_state_ = std::make_unique<RequestState>();
        request_state_->completion_event.reset();
    }

    HttpClient::RequestState *HttpClient::request_state() const
    {
        return request_state_.get();
    }

    void HttpClient::fail_or_complete_request(bool failed)
    {
        auto *state = request_state();
        if (!state) {
            return;
        }

        if (coroutine_request_mode_ && failed && !state->request_completed) {
            state->request_failed = true;
        }

        if (state->coroutine_waiting_response) {
            state->completion_event.notify();
        } else {
            exit();
        }
    }

    void HttpClient::clear_runtime_binding()
    {
        if (auto *state = request_state()) {
            state->event_loop = nullptr;
            state->timer_manager = nullptr;
        }
    }

    HttpResponseSnapshot HttpClient::snapshot_response(HttpResponse *response)
    {
        HttpResponseSnapshot snapshot;
        if (!response) {
            return snapshot;
        }

        snapshot.good = response->good();
        snapshot.response_code = response->get_response_code();
        snapshot.content_type = response->get_content_type();
        snapshot.downloading = response->is_downloading();
        snapshot.original_file_name = response->get_original_file_name();
        snapshot.headers = response->headers();
        if (const char *begin = response->body_begin()) {
            snapshot.body.assign(begin, response->get_body_length());
        }

        return snapshot;
    }

    bool HttpClient::connect(connected_callback ccb, request_function rcb)
    {
        if (!ccb || !rcb) {
            LOG_ERROR("must set callback!");
            return false;
        }

        Connection *conn = create_connection();
        if (!conn) {
            return false;
        }

        timer::WheelTimerManager manager;
        start_request_state();
        request_state_->conn_timer = timer::TimerUtil::build_timeout_timer(
            &manager, config::connection_idle_timeout, this, &HttpClient::on_timer);

#ifdef _WIN32
        SelectPoller poller;
#else
        EpollPoller poller;
#endif
        net::EventLoop loop(&poller, &manager);

        coroutine_request_mode_ = false;
        bind_runtime(conn, &loop, &manager, std::move(ccb), std::move(rcb));
        loop.loop();
        clear_runtime_binding();
        
        return true;
    }

    yuan::coroutine::Task<HttpResponse *> HttpClient::connect_async(
        yuan::coroutine::RuntimeView runtime,
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        if (!ccb) {
            LOG_ERROR("must set connected callback!");
            co_return nullptr;
        }

        auto *loop = runtime.event_loop();
        auto *timer_manager = runtime.timer_manager();
        if (!loop || !timer_manager) {
            LOG_ERROR("runtime must provide event loop and timer manager");
            co_return nullptr;
        }

        Connection *conn = create_connection();
        if (!conn) {
            co_return nullptr;
        }

        start_request_state();
        request_state_->conn_timer = timer::TimerUtil::build_timeout_timer(
            timer_manager, config::connection_idle_timeout, this, &HttpClient::on_timer);

        coroutine_request_mode_ = true;
        bind_runtime(conn, loop, timer_manager, std::move(ccb), nullptr);

        const auto connect_exit_reason = co_await yuan::coroutine::wait_connected(runtime, conn);

        auto *state = request_state();
        if (connect_exit_reason != net::EventLoopExitReason::coroutine_resume_requested || !state || !state->session) {
            clear_runtime_binding();
            coroutine_request_mode_ = false;
            co_return nullptr;
        }

        HttpResponse *response = nullptr;
        if (!state->request_completed) {
            const uint32_t wait_timeout = timeout_ms > 0 ? timeout_ms : config::connection_idle_timeout;
            state->coroutine_waiting_response = true;
            const bool timed_out = co_await state->completion_event.wait_for(state->timer_manager, wait_timeout);
            state->coroutine_waiting_response = false;
            if (!timed_out && !state->request_failed && state->request_completed && state->session) {
                response = state->session->get_context()->get_response();
            }
        } else if (state->session) {
            response = state->session->get_context()->get_response();
        }

        if (state->session && state->session->get_context() && state->session->get_context()->get_connection()) {
            state->session->get_context()->get_connection()->close();
        }

        clear_runtime_binding();
        coroutine_request_mode_ = false;
        co_return response;
    }

    yuan::coroutine::Task<HttpResponse *> HttpClient::connect_async(
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        if (!ccb) {
            LOG_ERROR("must set connected callback!");
            co_return nullptr;
        }

        timer::WheelTimerManager manager;

#ifdef _WIN32
        SelectPoller poller;
#else
        EpollPoller poller;
#endif
        net::EventLoop loop(&poller, &manager);
        yuan::coroutine::RuntimeView runtime(&loop, &manager);

        co_return yuan::coroutine::sync_wait(
            runtime,
            connect_async(runtime, std::move(ccb), timeout_ms));
    }

    yuan::coroutine::Task<HttpResponseSnapshot> HttpClient::connect_snapshot_async(
        yuan::coroutine::RuntimeView runtime,
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        co_return snapshot_response(co_await connect_async(runtime, std::move(ccb), timeout_ms));
    }

    yuan::coroutine::Task<HttpResponseSnapshot> HttpClient::connect_snapshot_async(
        connected_callback ccb,
        uint32_t timeout_ms)
    {
        co_return snapshot_response(co_await connect_async(std::move(ccb), timeout_ms));
    }

    void HttpClient::on_timer(timer::Timer *timer)
    {
        LOG_WARN("connect timeout, closing connection");
        fail_or_complete_request(true);
    }

    void HttpClient::exit()
    {
        if (auto *state = request_state()) {
            if (state->event_loop) {
                state->event_loop->quit();
            }
        }
    }
}
