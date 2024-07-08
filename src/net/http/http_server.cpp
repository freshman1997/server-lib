#include "net/base/poller/select_poller.h"
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <winnls.h>
#include <winnt.h>
#else
#include <unistd.h>
#endif

#include "timer/wheel_timer_manager.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/event/event_loop.h"
#include "net/http/http_server.h"
#include "net/base/socket/socket.h"
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
#include "net/http/header_util.h"
#include "net/http/proxy.h"

#ifdef _WIN32
std::string UTF8ToGBEx(const char *utf8)
{
    if (!utf8 || strlen(utf8) < 1)
        return "";

    std::stringstream ss;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    auto *wstr = new wchar_t[len + 1];
    memset(wstr, 0, len + 1);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, len);
    len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    auto *str = new char[len + 1];
    memset(str, 0, len + 1);
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, str, len, nullptr, nullptr);
    ss << str;
    delete[]wstr;
    delete[]str;
    return ss.str();
}
#endif // _WIN32


namespace net::http
{
    HttpServer::HttpServer() : quit_(false), state_(State::invalid), acceptor_(nullptr), event_loop_(nullptr), timer_manager_(nullptr), proxy_(nullptr)
    {
        
    }

    HttpServer::~HttpServer()
    {
        for (const auto &it : sessions_) {
            delete it.second;
        }

        sessions_.clear();

        if (proxy_) {
            delete proxy_;
        }

        if (acceptor_) {
            delete acceptor_;
        }
    }

    void HttpServer::on_connected(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        sessions_[sessionId] = new HttpSession(sessionId, new HttpSessionContext(conn), timer_manager_);
    }

    void HttpServer::on_error(Connection *conn)
    {
        
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
            if (proxy_ && proxy_->is_proxy(context->get_request()->get_raw_url())) {
                proxy_->serve_proxy(context->get_request(), context->get_response());
            } else {
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
            }

            if (config::close_idle_connection && sessions_.count(sessionId)) {
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
            delete sock;
            return false;
        }

        sock->set_reuse(true);
        sock->set_none_block(true);
        if (!sock->bind()) {
            std::cout << " bind port " << port << " failed !!" << std::endl;
            delete sock;
            return false;
        }

        acceptor_ = new TcpAcceptor(sock);
        if (!acceptor_->listen()) {
            std::cout << " listen failed " << std::endl;
            delete acceptor_;
            return false;
        }

        config::load_config();
        load_static_paths();

        return true;
    }

    void HttpServer::serve()
    {
        timer::WheelTimerManager timerManager;
        timer_manager_ = &timerManager;

        net::SelectPoller poller;
        net::EventLoop loop(&poller, &timerManager);
        acceptor_->set_event_handler(&loop);

        loop.update_event(acceptor_->get_channel());
        this->event_loop_ = &loop;
        loop.set_connection_handler(this);

        const auto &proxiesCfg = singleton::Singleton<HttpConfigManager>().get_type_array_properties<nlohmann::json>("proxies");
        if (!proxiesCfg.empty()) {
            proxy_ = new HttpProxy(this);
            if (!proxy_->load_proxy_config_and_init()) {
                return;
            }
        }

        std::cout << singleton::Singleton<HttpConfigManager>().get_string_property("server_name", "web server") << " started\n";

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
            if (proxy_) {
                proxy_->on_client_close(conn);
            }

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
        const int prefixIdx = -dispatcher_.get_compress_trie().find_prefix(url, true);
        const std::string prefix = url.substr(0, prefixIdx);
        const std::string &pathPrefix = static_paths_[prefix];
        if (pathPrefix.empty()) {
            resp->process_error();
            return;
        }

        if (prefixIdx + 1 > url.size()) {
            resp->process_error(ResponseCode::not_found);
            return;
        }

        const std::string &fileRelativePath = url.substr(prefixIdx + 1);
        const std::size_t pos = fileRelativePath.find_last_of('.');
        if (pos == std::string::npos) {
            resp->process_error();
            return;
        }

        const std::string &ext = fileRelativePath.substr(pos);
        if (!play_types_.count(ext)) {
            resp->process_error();
            return;
        }

        std::string path;
        if (pathPrefix[pathPrefix.size() - 1] == '/') {
            path = pathPrefix + fileRelativePath;
        } else {
            path = pathPrefix + "/" + fileRelativePath;
        }

        auto *session = req->get_context()->get_session();
        const SessionItem *data = session->get_session_value(fileRelativePath);
        if (const SessionItem *last = session->get_session_value("last"); last && data && last->number.pval != data->number.pval) {
            auto *stream = static_cast<std::fstream *>(last->number.pval);
            stream->close();
            session->remove_session_value(fileRelativePath);
            delete stream;
        }

        std::ifstream *fileStream;
        if (!data) {
#ifdef _WIN32
            path = UTF8ToGBEx(path.c_str());
#endif
            auto *file_ = new std::ifstream(path.c_str(), std::ios::binary);
            if (!file_->good()) {
                std::cout << "open file fail!\n";
                resp->process_error(ResponseCode::not_found);
                file_->close();
                delete file_;
                return;
            }

            session->add_session_value(fileRelativePath, file_);
            session->add_session_value("last", file_);
            fileStream = file_;
            session->set_close_call_back([fileRelativePath](HttpSession *httpSession) {
                if (const SessionItem *item = httpSession->get_session_value(fileRelativePath)) {
                    auto *stream = static_cast<std::ifstream *>(item->number.pval);
                    stream->close();
                    httpSession->remove_session_value(fileRelativePath);
                    delete stream;
                }
            });
        } else {
            fileStream = static_cast<std::ifstream *>(data->number.pval);
        }

        const std::size_t contentSize = config::client_max_content_length;
        std::size_t length;
        const std::string lenKey = "_length";
        if (const SessionItem *lenItem = session->get_session_value(lenKey); !lenItem) {
            fileStream->seekg(0, std::ios_base::end);
            length = fileStream->tellg();
            session->add_session_value(lenKey, length);
        } else {
            length = lenItem->number.sz_val;
        }

        const std::string &contentType = get_content_type(ext);
        if (length == 0 || contentType.empty()) {
            resp->process_error(ResponseCode::bad_request);
            return;
        }

        std::size_t offset = 0;

        if (const std::string *range = req->get_header(net::http::http_header_key::range)) {
            const auto &ranges = helper::parse_range(*range);
            if (ranges.empty()) {
                resp->process_error(ResponseCode::bad_request);
                return;
            }
            offset = ranges[0].first;
        }

        std::string bytes = "bytes ";
        bytes.append(std::to_string(offset))
            .append("-")
            .append(std::to_string(length - 1))
            .append("/")
            .append(std::to_string(length));
        
        resp->add_header("Content-Type", contentType);
        resp->add_header("Content-Range", bytes);

        std::size_t sz = contentSize + offset > length ? length - offset : contentSize;
        resp->add_header("Content-length", std::to_string(sz));

        fileStream->seekg(offset, std::ios::beg);
        if (resp->get_buff()->writable_size() < sz) {
            resp->get_buff()->resize(sz);
        }

        fileStream->read(resp->get_buff()->buffer_begin(), sz);
        resp->get_buff()->fill(sz);

        if (fileStream->eof()) {
            fileStream->clear();
        }

        resp->add_header("Accept-Ranges", "bytes");
        resp->set_response_code(net::http::ResponseCode::partial_content);
        resp->send();
    }
}