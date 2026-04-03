#include "server/context.h"
#include "server/server_session.h"
#include "server/command.h"
#include "handler/ftp_app.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"
#include "timer/timer.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace yuan;
using namespace yuan::net;
using namespace yuan::net::ftp;

namespace
{
    class FakeTimer : public timer::Timer
    {
    public:
        bool ready() override { return false; }
        void cancel() override { cancelled_ = true; }
        void reset() override {}
        bool is_processing() override { return false; }
        bool is_done() override { return false; }
        bool is_cancel() override { return cancelled_; }
        timer::TimerTask * get_task() override { return nullptr; }
    private:
        bool cancelled_ = false;
    };

    class FakeTimerManager : public timer::TimerManager
    {
    public:
        timer::Timer * timeout(uint32_t milliseconds, timer::TimerTask *task) override { (void)milliseconds; (void)task; timers_.push_back(std::make_unique<FakeTimer>()); return timers_.back().get(); }
        timer::Timer * interval(uint32_t timeout, uint32_t interval, timer::TimerTask *task, int32_t period = 0) override { (void)timeout; (void)interval; (void)task; (void)period; timers_.push_back(std::make_unique<FakeTimer>()); return timers_.back().get(); }
        bool schedule(timer::Timer *timer) override { (void)timer; return true; }
        void tick() override {}
        uint32_t get_time_unit() override { return 1; }
    private:
        std::vector<std::unique_ptr<FakeTimer>> timers_;
    };

    class FakeEventHandler : public EventHandler
    {
    public:
        void on_new_connection(Connection *conn) override { (void)conn; }
        void close_channel(Channel *channel) override { (void)channel; }
        void update_channel(Channel *channel) override { (void)channel; }
        void quit() override {}
        void queue_in_loop(std::function<void()> cb) override { (void)cb; }
    };

    class FakeApp : public FtpApp
    {
    public:
        bool is_ok() override { return true; }
        timer::TimerManager * get_timer_manager() override { return &timer_manager_; }
        EventHandler * get_event_handler() override { return &event_handler_; }
        void on_session_closed(FtpSession *session) override { (void)session; }
        void quit() override {}
    private:
        FakeTimerManager timer_manager_;
        FakeEventHandler event_handler_;
    };

    class FakeConnection : public Connection
    {
    public:
        FakeConnection() : state_(ConnectionState::connected), remote_("127.0.0.1", 2121), handler_(nullptr), event_handler_(nullptr) {}
        ConnectionState get_connection_state() override { return state_; }
        bool is_connected() override { return state_ == ConnectionState::connected; }
        const InetAddress & get_remote_address() override { return remote_; }
        void write(::yuan::buffer::Buffer *buff) override { if (buff) { output_buffer_.append_buffer(buff); } }
        void write_and_flush(::yuan::buffer::Buffer *buff) override { write(buff); flush(); }
        void flush() override {}
        void abort() override { output_buffer_.clear(); }
        void close() override { state_ = ConnectionState::closed; }
        ConnectionType get_conn_type() override { return ConnectionType::TCP; }
        Channel * get_channel() override { return nullptr; }
        void set_connection_handler(ConnectionHandler *handler) override { handler_ = handler; }
        ConnectionHandler * get_connection_handler() override { return handler_; }
        void forward(Connection *conn) override { (void)conn; }
        void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler) override { (void)sslHandler; }
        void on_read_event() override {}
        void on_write_event() override {}
        void set_event_handler(EventHandler *eventHandler) override { event_handler_ = eventHandler; }
    private:
        ConnectionState state_;
        InetAddress remote_;
        ConnectionHandler *handler_;
        EventHandler *event_handler_;
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
    const auto root = fs::temp_directory_path() / "ftp_command_tests_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "sub", ec);
    { std::ofstream(root / "sample.txt") << "hello"; std::ofstream(root / "rename.txt") << "rename"; }

    ServerContext::get_instance()->set_server_work_dir(root.generic_string());

    FakeApp app;
    auto *conn = new FakeConnection();
    auto *session = new ServerFtpSession(conn, &app, true);

    auto factory = CommandFactory::get_instance();
    require(factory->find_command("USER") != nullptr, "USER command should be registered");
    require(factory->find_command("MKD") != nullptr, "MKD command should be registered");
    require(factory->find_command("RNFR") != nullptr, "RNFR command should be registered");
    require(factory->find_command("HELP") != nullptr, "HELP command should be registered");

    auto resp = factory->find_command("USER")->execute(session, "tester");
    require(resp.code_ == FtpResponseCode::__331__, "USER should request password");
    resp = factory->find_command("PASS")->execute(session, "secret");
    require(resp.code_ == FtpResponseCode::__230__, "PASS should login");
    require(session->login_success(), "session should be logged in");

    resp = factory->find_command("MKD")->execute(session, "work");
    require(resp.code_ == FtpResponseCode::__257__, "MKD should create directory");
    require(fs::is_directory(root / "work"), "MKD should create the directory on disk");

    resp = factory->find_command("CWD")->execute(session, "work");
    require(resp.code_ == FtpResponseCode::__250__, "CWD should enter directory");
    require(session->get_cwd() == "/work", "CWD should update session cwd");

    resp = factory->find_command("CDUP")->execute(session, "");
    require(resp.code_ == FtpResponseCode::__250__, "CDUP should work");
    require(session->get_cwd() == "/", "CDUP should return to root");

    resp = factory->find_command("SIZE")->execute(session, "sample.txt");
    require(resp.code_ == FtpResponseCode::__213__ && resp.body_ == "5", "SIZE should return sample file size");

    resp = factory->find_command("STAT")->execute(session, "sample.txt");
    require(resp.code_ == FtpResponseCode::__213__, "STAT file should return file status");

    resp = factory->find_command("REST")->execute(session, "128");
    require(resp.code_ == FtpResponseCode::__350__, "REST should accept restart offset");
    require(session->get_item_value<int32_t>("restart_offset") == 128, "REST should store restart offset");

    resp = factory->find_command("ALLO")->execute(session, "256");
    require(resp.code_ == FtpResponseCode::__200__, "ALLO should accept allocation size");
    require(session->get_item_value<int32_t>("upload_file_size") == 256, "ALLO should store upload size");

    resp = factory->find_command("RNFR")->execute(session, "rename.txt");
    require(resp.code_ == FtpResponseCode::__350__, "RNFR should mark rename source");
    resp = factory->find_command("RNTO")->execute(session, "renamed.txt");
    require(resp.code_ == FtpResponseCode::__250__, "RNTO should rename file");
    require(fs::exists(root / "renamed.txt"), "RNTO should produce renamed file");

    resp = factory->find_command("DELE")->execute(session, "renamed.txt");
    require(resp.code_ == FtpResponseCode::__250__, "DELE should remove file");
    require(!fs::exists(root / "renamed.txt"), "DELE should delete the file");

    resp = factory->find_command("MODE")->execute(session, "S");
    require(resp.code_ == FtpResponseCode::__200__, "MODE S should be supported");
    resp = factory->find_command("STRU")->execute(session, "F");
    require(resp.code_ == FtpResponseCode::__200__, "STRU F should be supported");
    resp = factory->find_command("PORT")->execute(session, "127,0,0,1,7,138");
    require(resp.code_ == FtpResponseCode::__502__, "PORT should be explicitly unsupported");
    resp = factory->find_command("HELP")->execute(session, "");
    require(resp.code_ == FtpResponseCode::__214__, "HELP should return supported command list");

    resp = factory->find_command("RMD")->execute(session, "work");
    require(resp.code_ == FtpResponseCode::__250__, "RMD should remove empty directory");
    require(!fs::exists(root / "work"), "RMD should remove the directory");

    delete session;
    fs::remove_all(root, ec);
    std::cout << "ftp command tests passed\n";
    return 0;
}
