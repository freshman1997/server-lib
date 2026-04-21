#ifndef __APP_H__
#define __APP_H__

#include "service.h"
#include "net/handler/connection_handler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace yuan::buffer
{
    class ByteBuffer;
}

namespace yuan::net
{
    class DatagramAcceptor;
    class StreamAcceptor;
    class Connection;
    class EventLoop;
    class Poller;
}

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::app
{
    enum class TransportProtocol
    {
        tcp,
        udp
    };

    struct AppOption
    {
        std::string ip = "0.0.0.0";
        uint16_t port = 0;
        TransportProtocol protocol = TransportProtocol::tcp;
        bool reuse_addr = true;
        bool non_block = true;
        std::size_t max_packet_size = 1024 * 1024 * 5;
    };

    class App : public Service, public net::ConnectionHandler
    {
    public:
        explicit App(const AppOption &option = {});
        virtual ~App();

    public:
        bool init() override;

        void start() override;

        void stop() override;

        // Compatibility with the old app entry naming.
        void launch()
        {
            start();
        }

        void exit()
        {
            stop();
        }

        bool is_running() const;

        const AppOption & get_option() const;

        net::EventLoop * get_event_loop() const;

        timer::TimerManager * get_timer_manager() const;

    public:
        void on_connected(const std::shared_ptr<net::Connection> &conn) override;

        void on_error(const std::shared_ptr<net::Connection> &conn) override;

        void on_read(const std::shared_ptr<net::Connection> &conn) override;

        void on_write(const std::shared_ptr<net::Connection> &conn) override;

        void on_close(const std::shared_ptr<net::Connection> &conn) override;

    protected:
        void set_option(const AppOption &option);

        virtual bool on_init();

        virtual void on_start();

        virtual void on_stop();

        virtual void on_packet(net::Connection &conn, const buffer::ByteBuffer &packet) = 0;

        virtual void on_connection_open(net::Connection &conn);

        virtual void on_connection_error(net::Connection &conn);

        virtual void on_connection_write(net::Connection &conn);

        virtual void on_connection_close(net::Connection &conn);

    private:
        class AppData;
        std::unique_ptr<AppData> data_;
    };
}

#endif
