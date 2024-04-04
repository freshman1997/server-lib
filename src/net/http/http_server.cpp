#include "net/base/poller/poll_poller.h"
#include <functional>
#include <iostream>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include<WinSock2.h>
#endif

#include "timer/wheel_timer_manager.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/event/event_loop.h"
#include "net/http/http_server.h"
#include "net/base/socket/socket.h"
#include "net/base/connection/connection.h"
#include "net/http/request.h"
#include "net/http/session.h"
#include "net/http/response_code.h"
#include "net/http/ops/config_manager.h"
#include "net/http/ops/option.h"
#include "singleton/singleton.h"
#include "net/base/poller/epoll_poller.h"

namespace net::http
{
    HttpServer::HttpServer()
    {
        
    }

    HttpServer::~HttpServer()
    {
        for (const auto &it : sessions_) {
            delete it.second;
        }

        sessions_.clear();
    }

    void HttpServer::on_connected(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        sessions_[sessionId] = new HttpSession(sessionId, new HttpSessionContext(conn),
             &singleton::Singleton<timer::WheelTimerManager>());
    }

    void HttpServer::on_error(Connection *conn)
    {
        free_session(conn);
    }

    void HttpServer::on_read(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            std::cout << "----------------> error\n";
            return;
        }

        auto session = it->second;
        auto context = session->get_context();
        if (!context->parse()) {
            if (context->has_error()) {
                context->process_error(context->get_error_code());
            }
            return;
        }

        if (context->has_error()) {
            context->process_error(context->get_error_code());
            return;
        }

        if (context->is_completed()) {
            if (!context->try_parse_request_content()) {
                context->process_error(ResponseCode::bad_request);
                return;
            }

            auto handler = dispatcher_.get_handler(context->get_request()->get_raw_url());
            if (handler) {
                handler(context->get_request(), context->get_response());
            } else {
                // 404
                context->process_error(ResponseCode::not_found);
            }

            if (sessions_.count(sessionId)) {
                session->reset_timer();
            }
        }
    }

    void HttpServer::on_write(Connection *conn)
    {
        
    }

    void HttpServer::on_close(Connection *conn)
    {
        free_session(conn);
        event_loop_->on_close(conn);
    }

    bool HttpServer::init(int port)
    {
        if (singleton::Singleton<HttpConfigManager>().good()) {
            std::cout << singleton::Singleton<HttpConfigManager>().get_string_property("server_name") << " starting...\n";
        }

        net::Socket *sock = new net::Socket("", port);
        if (!sock->valid()) {
            std::cout << "create socket fail!!\n";
            return false;
        }

        sock->set_reuse(true);
        sock->set_none_block(true);
        if (!sock->bind()) {
            std::cout << " bind failed " << std::endl;
            return false;
        }

        acceptor_ = new TcpAcceptor(sock);
        if (!acceptor_->listen()) {
            std::cout << " listen failed " << std::endl;
            return false;
        }

        load_static_paths();

        return true;
    }

    void HttpServer::serve()
    {
        net::EventLoop loop(&singleton::Singleton<net::PollPoller>(), &singleton::Singleton<timer::WheelTimerManager>());
        acceptor_->set_event_handler(&loop);

        loop.update_event(acceptor_->get_channel());
        this->event_loop_ = &loop;
        loop.set_connection_handler(this);
        loop.loop();
    }

    void HttpServer::stop()
    {
        event_loop_->quit();
    }

    void HttpServer::on(const std::string &url, request_function func)
    {
        if (url.empty() || !func) {
            return;
        }

        dispatcher_.register_handler(url, func);
    }

    void HttpServer::free_session(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            delete it->second;
            sessions_.erase(it);
        } else {
            std::cerr << "internal error found!!!\n";
        }
    }

    void HttpServer::load_static_paths()
    {
        // 必须要支持 /static/* 这种形式
        auto &cfgManager = singleton::Singleton<HttpConfigManager>();
        const std::vector<nlohmann::json> &paths = cfgManager.get_type_array_properties<nlohmann::json>(config::static_file_paths);
        for (const auto &path : paths) {
            const std::string &root = path[config::static_file_paths_root];
            const std::string &rootPath = path[config::static_file_paths_path];
            if (root.empty() || rootPath.empty()) {
                continue;
            }

            on(root, std::bind(&HttpServer::serve_static, this, std::placeholders::_1, std::placeholders::_2));
        }

        const std::vector<std::string> &types = cfgManager.get_type_array_properties<std::string>(config::playable_types);
        for (const auto &type : types) {
            play_types_.insert(type);
        }
    }

    void HttpServer::serve_static(HttpRequest *req, HttpResponse *resp)
    {

    }
}