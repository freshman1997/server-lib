#include "server/context.h"
#include "server/server_session.h"
#include "server/command.h"
#include "handler/ftp_app.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

using namespace yuan;
using namespace yuan::net;
using namespace yuan::net::ftp;

namespace
{
    class FakeApp : public FtpApp
    {
    public:
        bool is_ok() override
        {
            return true;
        }
        NetworkRuntime *get_runtime() override
        {
            return &runtime_;
        }
        void on_session_closed(FtpSession *session) override
        {
            (void)session;
        }
        void quit() override
        {
        }

    private:
        NetworkRuntime runtime_;
    };

    class FakeConnection : public Connection
    {
    public:
        FakeConnection()
            : state_(ConnectionState::connected), remote_("127.0.0.1", 50000), local_("127.0.0.1", 2121)
        {
        }

        ConnectionState get_connection_state() const override
        {
            return state_;
        }
        bool is_connected() const override
        {
            return state_ == ConnectionState::connected;
        }
        const InetAddress &get_remote_address() const override
        {
            return remote_;
        }
        const InetAddress &get_local_address() const override
        {
            return local_;
        }
        void write(const ::yuan::buffer::ByteBuffer &buffer) override
        {
            if (!buffer.empty()) {
                append_output(buffer);
            }
        }
        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer) override
        {
            write(buffer);
            flush();
        }
        void flush() override
        {
        }
        void abort() override
        {
            output_buffer_.clear();
        }
        void close() override
        {
            state_ = ConnectionState::closed;
        }
        void set_connection_handler(std::shared_ptr<ConnectionHandler> handler) override
        {
            handler_owner_ = std::move(handler);
            handler_ = handler_owner_.get();
        }
        ConnectionHandler *get_connection_handler() const override
        {
            return handler_;
        }
        std::shared_ptr<ConnectionHandler> get_connection_handler_owner() const override
        {
            return handler_owner_;
        }
        void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler) override
        {
            (void)sslHandler;
        }
        void on_read_event() override
        {
        }
        void on_write_event() override
        {
        }
        void set_event_handler(EventHandler *eventHandler) override
        {
            event_handler_ = eventHandler;
        }

    private:
        ConnectionState state_;
        InetAddress remote_;
        InetAddress local_;
        std::shared_ptr<ConnectionHandler> handler_owner_;
        ConnectionHandler *handler_ = nullptr;
        EventHandler *event_handler_ = nullptr;
    };

    void require(bool cond, const std::string &message)
    {
        if (!cond) {
            throw std::runtime_error(message);
        }
    }
}

int main()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "ftp_epsv_eprt_tests_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    ServerContext::get_instance()->set_server_work_dir(root.generic_string());

    FakeApp app;
    auto *conn = new FakeConnection();
    auto *session = new ServerFtpSession(conn, &app, true);

    auto factory = CommandFactory::get_instance();
    auto resp = factory->find_command("USER")->execute(session, "tester");
    require(resp.code_ == FtpResponseCode::__331__, "USER should request password");
    resp = factory->find_command("PASS")->execute(session, "secret");
    require(resp.code_ == FtpResponseCode::__230__, "PASS should login");

    resp = factory->find_command("EPSV")->execute(session, "");
    require(resp.code_ == FtpResponseCode::__229__, "EPSV should return 229");
    require(session->get_passive_addr().has_value(), "EPSV should set passive address");

    resp = factory->find_command("EPRT")->execute(session, "|1|127.0.0.1|50021|");
    require(resp.code_ == FtpResponseCode::__200__, "EPRT should accept valid endpoint");
    require(session->get_active_addr().has_value(), "EPRT should set active address");
    require(session->get_active_addr()->get_ip() == "127.0.0.1", "EPRT ip should parse correctly");
    require(session->get_active_addr()->get_port() == 50021, "EPRT port should parse correctly");

    resp = factory->find_command("EPRT")->execute(session, "|3|127.0.0.1|50021|");
    require(resp.code_ == FtpResponseCode::__501__, "EPRT should reject unsupported AF");

    resp = factory->find_command("EPRT")->execute(session, "bad");
    require(resp.code_ == FtpResponseCode::__501__, "EPRT should reject invalid syntax");

    delete session;
    fs::remove_all(root, ec);
    return 0;
}
