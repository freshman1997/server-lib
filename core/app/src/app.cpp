#include "app.h"

#include "buffer/byte_buffer.h"
#include "event/event_loop.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/connection/connection.h"
#include "net/poller/poller.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"

#include <atomic>
#include <memory>

namespace yuan::app
{
    namespace
    {
        template<typename T>
        T *ptr_of(std::unique_ptr<T> &owner)
        {
            return owner ? &*owner : nullptr;
        }

        template<typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? &*owner : nullptr;
        }

    }

    class App::AppData
    {
    public:
        void reset_runtime_resources()
        {
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }
            stream_acceptor_ = nullptr;
            datagram_acceptor_ = nullptr;
            loop_.reset();
            poller_.reset();
            timer_manager_.reset();
            running_.store(false);
            initialized_.store(false);
        }

        AppOption option_;
        std::atomic_bool initialized_ = false;
        std::atomic_bool running_ = false;
        std::unique_ptr<net::Poller> poller_;
        std::unique_ptr<net::EventLoop> loop_;
        std::unique_ptr<timer::TimerManager> timer_manager_;
        std::unique_ptr<net::Acceptor> acceptor_;
        net::StreamAcceptor *stream_acceptor_ = nullptr;
        net::DatagramAcceptor *datagram_acceptor_ = nullptr;
    };

    App::App(const AppOption &option) : data_(std::make_unique<AppData>())
    {
        data_->option_ = option;
    }

    App::~App()
    {
        stop();
        data_->loop_.reset();
        data_->poller_.reset();
        data_->timer_manager_.reset();
    }

    bool App::init()
    {
        if (data_->initialized_.load()) {
            return true;
        }

        data_->reset_runtime_resources();

        if (data_->option_.port == 0) {
            return false;
        }

        data_->timer_manager_ = std::make_unique<timer::WheelTimerManager>();
        data_->poller_.reset(net::NetworkRuntime::create_default_poller());
        if (!data_->poller_ || !data_->poller_->init()) {
            data_->reset_runtime_resources();
            return false;
        }

        data_->loop_ = std::make_unique<net::EventLoop>(ptr_of(data_->poller_), ptr_of(data_->timer_manager_));

        auto socket = std::make_unique<net::Socket>(
            data_->option_.ip.c_str(),
            data_->option_.port,
            data_->option_.protocol == TransportProtocol::udp);
        if (!socket->valid()) {
            data_->reset_runtime_resources();
            return false;
        }

        socket->set_reuse(data_->option_.reuse_addr);
        socket->set_none_block(data_->option_.non_block);
        if (!socket->bind()) {
            data_->reset_runtime_resources();
            return false;
        }

        if (data_->option_.protocol == TransportProtocol::udp) {
            data_->acceptor_.reset(net::create_datagram_acceptor(socket.release(), ptr_of(data_->timer_manager_)));
            data_->datagram_acceptor_ = static_cast<net::DatagramAcceptor *>(ptr_of(data_->acceptor_));
        } else {
            data_->acceptor_.reset(net::create_stream_acceptor(socket.release()));
            data_->stream_acceptor_ = static_cast<net::StreamAcceptor *>(ptr_of(data_->acceptor_));
        }

        if (!data_->acceptor_->listen()) {
            data_->reset_runtime_resources();
            return false;
        }

        data_->acceptor_->set_connection_handler(net::make_non_owning_handler(*this));
        data_->acceptor_->set_event_handler(ptr_of(data_->loop_));

        if (data_->stream_acceptor_) {
            data_->loop_->update_channel(data_->stream_acceptor_->listener_channel());
        } else if (data_->datagram_acceptor_) {
            data_->loop_->update_channel(data_->datagram_acceptor_->endpoint_channel());
        }

        if (!on_init()) {
            data_->reset_runtime_resources();
            return false;
        }

        data_->initialized_.store(true);
        return true;
    }

    void App::start()
    {
        if (!init() || !data_->loop_) {
            return;
        }

        data_->running_.store(true);
        on_start();
        data_->loop_->loop();
        data_->running_.store(false);
        on_stop();
    }

    void App::stop()
    {
        if (data_->loop_) {
            data_->loop_->quit();
        }

        if (data_->acceptor_) {
            data_->acceptor_->close();
            data_->acceptor_.reset();
        }
        data_->stream_acceptor_ = nullptr;
        data_->datagram_acceptor_ = nullptr;

        data_->running_.store(false);
        data_->initialized_.store(false);
    }

    bool App::is_running() const
    {
        return data_->running_.load();
    }

    const AppOption & App::get_option() const
    {
        return data_->option_;
    }

    net::EventLoop * App::get_event_loop() const
    {
        return ptr_of(data_->loop_);
    }

    timer::TimerManager * App::get_timer_manager() const
    {
        return ptr_of(data_->timer_manager_);
    }

    void App::on_connected(const std::shared_ptr<net::Connection> &conn)
    {
        if (!conn) {
            return;
        }

        conn->set_max_packet_size(data_->option_.max_packet_size);
        on_connection_open(*conn);
    }

    void App::on_error(const std::shared_ptr<net::Connection> &conn)
    {
        if (conn) {
            on_connection_error(*conn);
        }
    }

    void App::on_read(const std::shared_ptr<net::Connection> &conn)
    {
        if (!conn) {
            return;
        }

        auto packet = conn->take_input_byte_buffer();
        if (packet.empty()) {
            return;
        }

        on_packet(*conn, packet);
    }

    void App::on_write(const std::shared_ptr<net::Connection> &conn)
    {
        if (conn) {
            on_connection_write(*conn);
        }
    }

    void App::on_close(const std::shared_ptr<net::Connection> &conn)
    {
        if (conn) {
            on_connection_close(*conn);
        }
    }

    void App::set_option(const AppOption &option)
    {
        data_->option_ = option;
    }

    bool App::on_init()
    {
        return true;
    }

    void App::on_start()
    {
    }

    void App::on_stop()
    {
    }

    void App::on_connection_open(net::Connection &conn)
    {
        (void)conn;
    }

    void App::on_connection_error(net::Connection &conn)
    {
        (void)conn;
    }

    void App::on_connection_write(net::Connection &conn)
    {
        (void)conn;
    }

    void App::on_connection_close(net::Connection &conn)
    {
        (void)conn;
    }
}
