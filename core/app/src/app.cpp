#include "app.h"

#include "buffer/byte_buffer.h"
#include "event/event_loop.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/connection/connection.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"

#ifdef __linux__
#include "net/poller/epoll_poller.h"
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include "net/poller/kqueue_poller.h"
#endif

#include <atomic>
#include <memory>

namespace yuan::app
{
    namespace
    {
        net::Poller *create_default_poller()
        {
        #ifdef __linux__
            return new net::EpollPoller;
        #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
            return new net::KQueuePoller;
        #else
            return new net::SelectPoller;
        #endif
        }

    }

    class App::AppData
    {
    public:
        AppOption option_;
        std::atomic_bool initialized_ = false;
        std::atomic_bool running_ = false;
        net::Poller *poller_ = nullptr;
        net::EventLoop *loop_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        net::Acceptor *acceptor_ = nullptr;
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

        if (data_->loop_) {
            delete data_->loop_;
            data_->loop_ = nullptr;
        }

        if (data_->poller_) {
            delete data_->poller_;
            data_->poller_ = nullptr;
        }

        if (data_->timer_manager_) {
            delete data_->timer_manager_;
            data_->timer_manager_ = nullptr;
        }
    }

    bool App::init()
    {
        if (data_->initialized_.load()) {
            return true;
        }

        if (data_->acceptor_) {
            data_->acceptor_->close();
            data_->acceptor_ = nullptr;
        }
        data_->stream_acceptor_ = nullptr;
        data_->datagram_acceptor_ = nullptr;

        if (data_->loop_) {
            delete data_->loop_;
            data_->loop_ = nullptr;
        }

        if (data_->poller_) {
            delete data_->poller_;
            data_->poller_ = nullptr;
        }

        if (data_->timer_manager_) {
            delete data_->timer_manager_;
            data_->timer_manager_ = nullptr;
        }

        data_->running_.store(false);
        data_->initialized_.store(false);

        if (data_->option_.port == 0) {
            return false;
        }

        data_->timer_manager_ = new timer::WheelTimerManager;
        data_->poller_ = create_default_poller();
        if (!data_->poller_ || !data_->poller_->init()) {
            stop();
            if (data_->loop_) {
                delete data_->loop_;
                data_->loop_ = nullptr;
            }
            if (data_->poller_) {
                delete data_->poller_;
                data_->poller_ = nullptr;
            }
            if (data_->timer_manager_) {
                delete data_->timer_manager_;
                data_->timer_manager_ = nullptr;
            }
            return false;
        }

        data_->loop_ = new net::EventLoop(data_->poller_, data_->timer_manager_);

        auto *socket = new net::Socket(data_->option_.ip.c_str(),
                                       data_->option_.port,
                                       data_->option_.protocol == TransportProtocol::udp);
        if (!socket->valid()) {
            delete socket;
            if (data_->loop_) {
                delete data_->loop_;
                data_->loop_ = nullptr;
            }
            if (data_->poller_) {
                delete data_->poller_;
                data_->poller_ = nullptr;
            }
            if (data_->timer_manager_) {
                delete data_->timer_manager_;
                data_->timer_manager_ = nullptr;
            }
            return false;
        }

        socket->set_reuse(data_->option_.reuse_addr);
        socket->set_none_block(data_->option_.non_block);
        if (!socket->bind()) {
            delete socket;
            if (data_->loop_) {
                delete data_->loop_;
                data_->loop_ = nullptr;
            }
            if (data_->poller_) {
                delete data_->poller_;
                data_->poller_ = nullptr;
            }
            if (data_->timer_manager_) {
                delete data_->timer_manager_;
                data_->timer_manager_ = nullptr;
            }
            return false;
        }

        if (data_->option_.protocol == TransportProtocol::udp) {
            data_->datagram_acceptor_ = net::create_datagram_acceptor(socket, data_->timer_manager_);
            data_->acceptor_ = data_->datagram_acceptor_;
        } else {
            data_->stream_acceptor_ = net::create_stream_acceptor(socket);
            data_->acceptor_ = data_->stream_acceptor_;
        }

        if (!data_->acceptor_->listen()) {
            data_->acceptor_ = nullptr;
            if (data_->loop_) {
                delete data_->loop_;
                data_->loop_ = nullptr;
            }
            if (data_->poller_) {
                delete data_->poller_;
                data_->poller_ = nullptr;
            }
            if (data_->timer_manager_) {
                delete data_->timer_manager_;
                data_->timer_manager_ = nullptr;
            }
            return false;
        }

        data_->acceptor_->set_connection_handler(this);
        data_->acceptor_->set_event_handler(data_->loop_);

        if (data_->stream_acceptor_) {
            data_->loop_->update_channel(data_->stream_acceptor_->listener_channel());
        } else if (data_->datagram_acceptor_) {
            data_->loop_->update_channel(data_->datagram_acceptor_->endpoint_channel());
        }

        if (!on_init()) {
            stop();
            if (data_->loop_) {
                delete data_->loop_;
                data_->loop_ = nullptr;
            }
            if (data_->poller_) {
                delete data_->poller_;
                data_->poller_ = nullptr;
            }
            if (data_->timer_manager_) {
                delete data_->timer_manager_;
                data_->timer_manager_ = nullptr;
            }
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
            data_->acceptor_ = nullptr;
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
        return data_->loop_;
    }

    timer::TimerManager * App::get_timer_manager() const
    {
        return data_->timer_manager_;
    }

    void App::on_connected(net::Connection *conn)
    {
        if (!conn) {
            return;
        }

        conn->set_max_packet_size(data_->option_.max_packet_size);
        on_connection_open(conn);
    }

    void App::on_error(net::Connection *conn)
    {
        on_connection_error(conn);
    }

    void App::on_read(net::Connection *conn)
    {
        if (!conn) {
            return;
        }

        auto packet = conn->take_input_byte_buffer();
        if (packet.empty()) {
            return;
        }

        on_packet(conn, packet);
    }

    void App::on_write(net::Connection *conn)
    {
        on_connection_write(conn);
    }

    void App::on_close(net::Connection *conn)
    {
        on_connection_close(conn);
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

    void App::on_connection_open(net::Connection *conn)
    {
    }

    void App::on_connection_error(net::Connection *conn)
    {
    }

    void App::on_connection_write(net::Connection *conn)
    {
    }

    void App::on_connection_close(net::Connection *conn)
    {
    }
}
