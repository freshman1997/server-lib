#include "base/time.h"
#include <fstream>
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
#include "net/http/response.h"
#include "net/http/content_type.h"
#include "net/http/header_key.h"

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
        net::EventLoop loop(&singleton::Singleton<net::EpollPoller>(), &singleton::Singleton<timer::WheelTimerManager>());
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

    void HttpServer::on(const std::string &url, request_function func, bool is_prefix)
    {
        if (url.empty() || !func) {
            return;
        }

        dispatcher_.register_handler(url, func, is_prefix);
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

            static_paths_[root] = rootPath;
            on(root, std::bind(&HttpServer::serve_static, this, std::placeholders::_1, std::placeholders::_2), true);
        }

        const std::vector<std::string> &types = cfgManager.get_type_array_properties<std::string>(config::playable_types);
        for (const auto &type : types) {
            play_types_.insert(type);
        }
    }

    void HttpServer::serve_static(HttpRequest *req, HttpResponse *resp)
    {
        const std::string &url = req->get_raw_url();
        int prefixIdx = -dispatcher_.get_compress_trie().find_prefix(url, true);
        std::string prefix = url.substr(0, prefixIdx);
        const std::string &pathPrefix = static_paths_[prefix];
        if (pathPrefix.empty()) {
            resp->get_context()->process_error();
            return;
        }

        if (prefixIdx + 1 > url.size()) {
            resp->get_context()->process_error(ResponseCode::not_found);
            return;
        }

        const std::string &fileRelativePath = url.substr(prefixIdx + 1);
        std::size_t pos = fileRelativePath.find_last_of('.');
        if (pos == std::string::npos) {
            resp->get_context()->process_error();
            return;
        }

        const std::string &ext = fileRelativePath.substr(pos);
        if (!play_types_.count(ext)) {
            resp->get_context()->process_error();
            return;
        }

        std::string path;
        if (pathPrefix[pathPrefix.size() - 1] == '/') {
            path = pathPrefix + fileRelativePath;
        } else {
            path = pathPrefix + "/" + fileRelativePath;
        }
        
        auto *session = req->get_context()->get_session();
        SessionItem *data = session->get_session_value(fileRelativePath);
        std::fstream *fileStream = nullptr;
        if (!data) {
            std::fstream *file_ = new std::fstream(path.c_str(), std::ios_base::in);
            if (!file_->good()) {
                std::cout << "open file fail!\n";
                resp->get_context()->process_error(ResponseCode::not_found);
                return;
            }

            session->add_session_value(fileRelativePath, file_);
            fileStream = file_;
            session->set_close_call_back([fileRelativePath](HttpSession *session) {
                SessionItem *data = session->get_session_value(fileRelativePath);
                if (data) {
                    std::fstream *stream = (std::fstream *) data->number.pval;
                    stream->close();
                    session->remove_session_value(fileRelativePath);
                    delete stream;
                }
            });
        } else {
            fileStream = (std::fstream *) data->number.pval;
        }
        
        int content_size_ = -1;
        fileStream->seekg(0, std::ios_base::end);
        std::size_t length_ = fileStream->tellg();
        const std::string &contentType = get_content_type(ext);
        if (length_ == 0 || contentType.empty()) {
            resp->get_context()->process_error(ResponseCode::bad_request);
            return;
        }

        content_size_ = 1024 * 1024;
        if (content_size_ < 0) {
            resp->get_context()->process_error();
            return;
        }

        const std::string *range = req->get_header(net::http::http_header_key::range);
        long long offset = 0;

        if (range) {
            size_t pos = range->find_first_of("=");
            if (std::string::npos == pos) {
                resp->get_context()->process_error();
                return;
            }

            size_t pos1 = range->find_first_of("-");
            offset = std::atol(range->substr(pos + 1, pos1 - pos).c_str());
        }

        std::string bytes = "bytes ";
        bytes.append(std::to_string(offset))
            .append("-")
            .append(std::to_string(length_ - 1))
            .append("/")
            .append(std::to_string(length_));
        
        resp->add_header("Content-Type", contentType);
        resp->add_header("Content-Range", bytes);

        size_t r = content_size_ + offset > length_ ? length_ - offset : content_size_;
        resp->add_header("Content-length", std::to_string(r));

        fileStream->seekg(offset, std::ios::beg);
        resp->get_buff()->reset();
        if (resp->get_buff()->writable_size() < content_size_) {
            resp->get_buff()->resize(content_size_);
        }

        fileStream->read(resp->get_buff()->buffer_begin(), content_size_);
        resp->get_buff()->fill(r);

        if (fileStream->eof()) {
            fileStream->clear();
        }

        resp->add_header("Accept-Ranges", "bytes");
        resp->set_response_code(net::http::ResponseCode::partial_content);
        resp->send();
    }
}