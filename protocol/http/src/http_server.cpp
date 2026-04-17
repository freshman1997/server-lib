#include "content/types.h"
#include "context.h"
#include "coroutine/io_result.h"
#include "net/runtime/network_runtime.h"
#include "net/secuity/openssl.h"
#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"
#include "task/save_upload_tmp_chunk_task.h"
#include "task/upload_file_task.h"
#include "url.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "logger.h"
#include <memory>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <winnls.h>
#include <winnt.h>
#else
#include <unistd.h>
#endif

#include "http_server.h"
#include "middleware.h"
#include "net/socket/socket.h"
#include "request.h"
#include "response.h"
#include "session.h"
#include "response_code.h"
#include "ops/config_manager.h"
#include "ops/option.h"
#include "header_key.h"
#include "header_util.h"
#include "proxy.h"

namespace yuan::net::http
{
    namespace
    {
        using HttpSessionMap = std::unordered_map<uint64_t, std::unique_ptr<HttpSession> >;

        HttpSession *find_http_session(HttpSessionMap &sessions, uint64_t session_id)
        {
            const auto it = sessions.find(session_id);
            return it == sessions.end() ? nullptr : it->second.get();
        }
    }

    HttpServer::HttpServer()
        : HttpServer(HttpServerConfig())
    {
    }

    HttpServer::HttpServer(const HttpServerConfig & config)
        : config_(config)
    {
        if (config_.enable_keep_alive) {
            global_pipeline_.add(middlewares::connection_handler());
        }
        if (config_.enable_cors) {
            global_pipeline_.add(middlewares::cors());
        }
        if (config_.max_body_size > 0) {
            global_pipeline_.add(middlewares::body_limit(config_.max_body_size));
        }
    }

    HttpServer::~HttpServer()
    {
        sessions_.clear();
    }

    bool HttpServer::init_ssl_if_needed()
    {
#ifdef HTTP_USE_SSL
        ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!ssl_module_->init("./ca/ca.crt", "./ca/ca.key", SSLHandler::SSLMode::acceptor_)) {
            if (auto msg = ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            return false;
        }

        listener_.set_ssl_module(ssl_module_);
#endif
        return true;
    }

    bool HttpServer::init_http_features()
    {
        try
        {
            config::load_config();
            load_static_paths();
            register_builtin_routes();
            if (!init_proxy_if_needed()) {
                return false;
            }

            thread_pool_ = std::make_unique<thread::ThreadPool>(config_.thread_pool_size);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Exception during server init: {}", e.what());
            thread_pool_ = std::make_unique<thread::ThreadPool>(config_.thread_pool_size);
        }
        catch (...)
        {
            LOG_ERROR("Unknown exception during server init");
            thread_pool_ = std::make_unique<thread::ThreadPool>(config_.thread_pool_size);
        }

        return true;
    }

    void HttpServer::register_builtin_routes()
    {
        on("/favicon.ico", HttpServer::icon);

        on("/reload_config", [this](HttpRequest *req, HttpResponse *resp) {
            reload_config(req, resp);
        });

        on("/upload", [this](HttpRequest *req, HttpResponse *resp) {
            serve_upload(req, resp);
        });
    }

    bool HttpServer::init_proxy_if_needed()
    {
        const auto &proxiesCfg = HttpConfigManager::get_instance()->get_type_array_properties<nlohmann::json>("proxies");
        if (proxiesCfg.empty()) {
            return true;
        }

        proxy_ = std::make_unique<HttpProxy>(this);
        if (!proxy_->load_proxy_config_and_init()) {
            LOG_ERROR("load proxies config failed!");
            proxy_.reset();
            return false;
        }

        return true;
    }

    bool HttpServer::parse_request(HttpSessionContext * context)
    {
        if (!context->parse()) {
            if (context->has_error()) {
                context->process_error(context->get_error_code());
            }
            return false;
        }

        if (context->has_error()) {
            context->process_error(context->get_error_code());
            return false;
        }

        if (!context->is_completed()) {
            return false;
        }

        if (!context->try_parse_request_content()) {
            context->process_error(ResponseCode::bad_request);
            return false;
        }

        return context->is_completed();
    }

    bool HttpServer::parse_request(HttpSessionContext * context, const ::yuan::buffer::ByteBuffer & data)
    {
        if (!context->parse_from(data)) {
            if (context->has_error()) {
                context->process_error(context->get_error_code());
            }
            return false;
        }

        if (context->has_error()) {
            context->process_error(context->get_error_code());
            return false;
        }

        if (!context->is_completed()) {
            return false;
        }

        if (!context->try_parse_request_content()) {
            context->process_error(ResponseCode::bad_request);
            return false;
        }

        return context->is_completed();
    }

    bool HttpServer::dispatch_request(HttpSessionContext * context)
    {
        auto *request = context->get_request();
        auto *response = context->get_response();

        if (request->is_options()) {
            handle_options_preflight(request, response);
            return true;
        }

        if (proxy_ && proxy_->is_proxy_url(request->get_raw_url())) {
            const auto &route_key = proxy_->find_proxy_route(request->get_raw_url());
            auto *upgrade = request->get_header("upgrade");
            bool is_ws_upgrade = false;
            if (upgrade) {
                std::string upgrade_lower = *upgrade;
                std::transform(upgrade_lower.begin(), upgrade_lower.end(), upgrade_lower.begin(), ::tolower);
                is_ws_upgrade = (upgrade_lower == "websocket");
            }
            if (is_ws_upgrade && ws_proxy_handler_) {
                auto *client_key_hdr = request->get_header("sec-websocket-key");
                std::string client_key = client_key_hdr ? *client_key_hdr : "";
                if (!client_key.empty()) {
                    std::string subproto;
                    auto *subproto_hdr = request->get_header("sec-websocket-protocol");
                    if (subproto_hdr && !subproto_hdr->empty()) {
                        subproto = *subproto_hdr;
                    }
                    context->ws_handoff_ = true;
                    context->ws_route_key_ = route_key;
                    context->ws_client_key_ = client_key;
                    context->ws_subproto_ = subproto;
                    return true;
                }
            }
            if (is_ws_upgrade) {
                proxy_->handle_websocket_upgrade_by_url(request, response, route_key);
            } else {
                proxy_->serve_proxy(request, response);
            }
            return true;
        }

        if (!global_pipeline_.empty() && !global_pipeline_.execute(request, response)) {
            return true;
        }

        if (const auto handler = dispatcher_.get_handler(request->get_raw_url())) {
            handler(request, response);
        } else {
            context->process_error(ResponseCode::not_found);
        }

        return true;
    }

    void HttpServer::finalize_request(uint64_t sessionId, HttpSession * session, HttpSessionContext * context)
    {
        if (!find_http_session(sessions_, sessionId)) {
            return;
        }

        if (config::close_idle_connection && session) {
            session->reset_timer();
        }
    }

    bool HttpServer::init(int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return init(port, *owned_runtime_);
    }

    bool HttpServer::init(int port, NetworkRuntime & runtime)
    {
        sessions_.clear();
        proxy_.reset();
        listener_.close();

        if (HttpConfigManager::get_instance()->good()) {
            LOG_INFO("{} starting...", HttpConfigManager::get_instance()->get_string_property("server_name"));
        }

        if (!init_ssl_if_needed()) {
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        if (!listener_.bind(port, runtime)) {
            LOG_ERROR("bind port {} failed!", port);
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        if (!init_http_features()) {
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        return true;
    }

    void HttpServer::serve()
    {
        LOG_INFO("{} started", HttpConfigManager::get_instance()->get_string_property("server_name", config_.server_name));

        if (thread_pool_) {
            thread_pool_->start();
        }

        listener_.set_connection_handler(
            [this](net::AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        if (owned_runtime_) {
            auto accept_task = listener_.run_async();
            accept_task.resume();
            owned_runtime_->run();
        }
    }

    void HttpServer::stop()
    {
        if (thread_pool_) {
            thread_pool_->shutdown();
        }
        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }

    void HttpServer::on(const std::string & url, request_function func, bool is_prefix)
    {
        if (url.empty() || !func)
            return;
        dispatcher_.register_handler(url, func, is_prefix);
    }

    void HttpServer::on(const std::string & url, request_function func,
                        std::shared_ptr<MiddlewarePipeline> pipeline, bool is_prefix)
    {
        if (url.empty() || !func || !pipeline)
            return;

        auto wrapped_func = [func, pipeline](HttpRequest *req, HttpResponse *resp) mutable {
            if (pipeline && !pipeline->execute(req, resp)) return;
            func(req, resp);
        };

        dispatcher_.register_handler(url, wrapped_func, is_prefix);
    }

    void HttpServer::use(std::shared_ptr<HttpMiddleware> middleware)
    {
        if (middleware)
            global_pipeline_.add(std::move(middleware));
    }

    void HttpServer::use(middleware_function fn, const char * name)
    {
        global_pipeline_.add(std::move(fn), name);
    }

    coroutine::Task<void> HttpServer::handle_connection(net::AsyncConnectionContext ctx)
    {
        auto *conn = ctx.native_handle();
        if (!conn) {
            co_return;
        }

        if (conn->is_ssl_handshaking()) {
            auto hs_result = co_await ctx.ssl_handshake_async();
            if (hs_result != coroutine::SslHandshakeResult::success) {
                co_return;
            }
        }

        const auto sessionId = ctx.connection_id();
        conn->set_max_packet_size(HttpPacket::get_max_packet_size());

        auto *httpCtx = new HttpSessionContext(conn);
        auto session = std::make_unique<HttpSession>(sessionId, httpCtx, listener_.runtime()->runtime_view());
        auto *session_ptr = session.get();
        sessions_[sessionId] = std::move(session);

        while (ctx.is_connected()) {
            auto read_result = co_await ctx.read_async();
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            auto *context = session->get_context();

            try
            {
                if (!parse_request(context, read_result.data)) {
                    if (context->has_error()) {
                        break;
                    }
                    continue;
                }

                (void)dispatch_request(context);

                if (context->ws_handoff_ && ws_proxy_handler_) {
                    std::string route_key = std::move(context->ws_route_key_);
                    std::string raw_url = context->get_request()->get_raw_url();
                    std::string client_key = std::move(context->ws_client_key_);
                    std::string subproto = std::move(context->ws_subproto_);
                    auto leftover = context->take_leftover_buffer();
                    sessions_.erase(sessionId);
                    delete session;

                    auto proxy_task = ws_proxy_handler_(std::move(ctx), raw_url, route_key, client_key, subproto, std::move(leftover));
                    proxy_task.resume();
                    proxy_task.detach();
                    co_return;
                }

                finalize_request(sessionId, session, context);

                while (context->get_response()->is_uploading()) {
                    auto flush_result = co_await ctx.flush_async();
                    if (flush_result.status != coroutine::IoStatus::success) {
                        break;
                    }
                    context->write();
                }
            }
            catch (const fmt::format_error &e)
            {
                LOG_ERROR("Invalid UTF-8 or format error while processing HTTP request: {}", e.what());
                if (find_http_session(sessions_, sessionId)) {
                    context->process_error(ResponseCode::bad_request);
                }
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Exception while processing HTTP request: {}", e.what());
                if (find_http_session(sessions_, sessionId)) {
                    context->process_error(ResponseCode::internal_server_error);
                }
            }
            catch (...)
            {
                LOG_ERROR("Unknown exception while processing HTTP request");
                if (find_http_session(sessions_, sessionId)) {
                    context->process_error(ResponseCode::internal_server_error);
                }
            }
        }

        if (proxy_ && conn) {
            proxy_->on_client_close(conn);
        }
        sessions_.erase(sessionId);
        delete session;

        co_return;
    }

    void HttpServer::load_static_paths()
    {
        auto cfgManager = HttpConfigManager::get_instance();
        const std::vector<nlohmann::json> &paths = cfgManager->get_type_array_properties<nlohmann::json>(config::static_file_paths);

        for (const auto &path : paths) {
            const std::string &root = path[config::static_file_paths_root];
            const std::string &rootPath = path[config::static_file_paths_path];
            if (root.empty() || rootPath.empty())
                continue;

            static_paths_[root] = rootPath;
            on(root, [this](HttpRequest *req, HttpResponse *resp) { 
                this->serve_static(req, resp);
                     },
               true);
        }

        if (paths.empty()) {
            static_paths_["/static"] = std::filesystem::current_path().string();
            on("/static", [this](HttpRequest *req, HttpResponse *resp) { 
                this->serve_static(req, resp);
                          },
               true);
        }

        const std::vector<std::string> &types = cfgManager->get_type_array_properties<std::string>(config::playable_types);
        for (const auto &type : types)
            play_types_.insert(type);

        if (play_types_.empty()) {
            play_types_.insert({ ".mp4", ".mp3", ".mov", ".flac", ".wav", ".avi", ".ogg" });
        }
    }

    void HttpServer::icon(HttpRequest * req, HttpResponse * resp)
    {
        (void)req;
        resp->add_header("Connection", "close");
        resp->add_header("Content-Type", "image/x-icon");
        resp->set_response_code(ResponseCode::ok_);

        std::fstream file;
        file.open("icon.ico");
        if (!file.good()) {
            resp->process_error(ResponseCode::not_found);
            return;
        }

        file.seekg(0, std::ios_base::end);
        std::size_t sz = file.tellg();
        if (sz == 0 || sz > config::client_max_content_length) {
            resp->process_error();
            return;
        }

        resp->reserve_body_buffer(sz);
        file.seekg(0, std::ios_base::beg);
        file.read(resp->body_write_ptr(), sz);
        resp->commit_body_bytes(sz);

        resp->add_header("Content-length", std::to_string(sz));
        resp->send();
    }

    bool HttpServer::resolve_static_request(
        const std::string & url,
        std::string & prefix,
        std::string & path_prefix,
        std::string & file_relative_path,
        HttpResponse * resp)
    {
        const auto result = dispatcher_.get_compress_trie().find_prefix(url);
        const auto prefix_idx = static_cast<int>(result.match_length);

        if (!result || prefix_idx <= 0) {
            resp->process_error(ResponseCode::not_found);
            return false;
        }

        prefix = url.substr(0, static_cast<size_t>(prefix_idx));
        auto static_path_it = static_paths_.find(prefix);
        if (static_path_it == static_paths_.end() || static_path_it->second.empty()) {
            resp->process_error(ResponseCode::not_found);
            return false;
        }

        path_prefix = static_path_it->second;
        if (url.size() <= static_cast<size_t>(prefix_idx) + 1) {
            file_relative_path.clear();
            return true;
        }

        file_relative_path = url.substr(static_cast<size_t>(prefix_idx) + 1);
        if (file_relative_path.find("..") != std::string::npos ||
            file_relative_path.find('\\') != std::string::npos ||
            (!file_relative_path.empty() && file_relative_path.front() == '/')) {
            resp->process_error(ResponseCode::forbidden);
            return false;
        }

        return true;
    }

    bool HttpServer::serve_embedded_static_page(const std::string & file_relative_path, HttpResponse * resp)
    {
        if (file_relative_path == "filelist.html") {
            resp->append_body(config::file_list_html_text);
        } else if (file_relative_path == "upload") {
            resp->append_body(config::upload_html_text);
        } else {
            return false;
        }

        resp->add_header("Content-Type", "text/html; charset=utf-8");
        resp->set_response_code(ResponseCode::ok_);
        resp->add_header("Connection", "close");
        resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
        resp->send();
        resp->get_context()->get_connection()->close();
        return true;
    }

    void HttpServer::serve_static_file(
        HttpRequest * req,
        HttpResponse * resp,
        const std::string & url,
        const std::string & file_relative_path,
        const std::string & path_prefix)
    {
        std::string path;
        if (!path_prefix.empty() && path_prefix.back() == '/') {
            path = path_prefix + file_relative_path;
        } else {
            path = path_prefix + "/" + file_relative_path;
        }

        try
        {
            if (std::filesystem::is_directory(std::filesystem::path(std::u8string(path.begin(), path.end())))) {
                serve_list_files(path_prefix, path, resp);
                return;
            }
        }
        catch (...)
        {
            resp->process_error(ResponseCode::forbidden);
            return;
        }

        const auto dot_pos = file_relative_path.find_last_of('.');
        const bool has_ext = dot_pos != std::string::npos && dot_pos > 0;
        if (!has_ext) {
            serve_download(path, "", resp);
            return;
        }

        const std::string ext = file_relative_path.substr(dot_pos);
        if (req->get_request_params().contains("justDownload")) {
            serve_download(path, ext, resp);
            return;
        }

#ifdef _WIN32
        std::ifstream stream(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::in | std::ios::binary);
#else
        std::ifstream stream(path.c_str(), std::ios::in | std::ios::binary);
#endif
        if (!stream.good()) {
            resp->process_error(ResponseCode::not_found);
            return;
        }

        stream.seekg(0, std::ios_base::end);
        const auto file_size = stream.tellg();
        if (file_size <= 0) {
            resp->process_error(ResponseCode::bad_request);
            return;
        }

        const auto length = static_cast<std::size_t>(file_size);
        const std::string &content_type = get_content_type(ext);
        if (content_type.empty()) {
            resp->process_error(ResponseCode::bad_request);
            return;
        }

        uint64_t offset = 0;
        bool has_range = false;
        if (const std::string *range = req->get_header(http_header_key::range)) {
            int ret = 0;
            const auto &ranges = helper::parse_range(*range, ret);
            if (ret == 0 && !ranges.empty()) {
                offset = ranges[0].first;
                if (offset >= length) {
                    resp->add_header("Content-Range", "bytes */" + std::to_string(length));
                    resp->set_response_code(ResponseCode::range_not_satisfiable);
                    resp->add_header("Content-Length", "0");
                    resp->send();
                    return;
                }
                has_range = true;
            }
        }

        const auto max_content_size = static_cast<std::size_t>(config::client_max_content_length);
        const auto remaining = length - static_cast<std::size_t>(offset);
        const auto sz = (std::min)(max_content_size, remaining);

        if (has_range) {
            const auto end_pos = offset + sz - 1;
            resp->add_header("Content-Range",
                             "bytes " + std::to_string(offset) + "-" +
                                 std::to_string(end_pos) + "/" + std::to_string(length));
            resp->set_response_code(ResponseCode::partial_content);
        } else {
            resp->set_response_code(ResponseCode::ok_);
        }

        resp->add_header("Content-Type", content_type);
        resp->add_header("Content-Disposition", "inline; filename=\"" + url::url_encode(file_relative_path) + "\"");
        resp->add_header("Content-length", std::to_string(sz));
        resp->add_header("Accept-Ranges", "bytes");
        resp->add_header("X-Content-Type-Options", "nosniff");
        resp->add_header("Cache-Control", "no-cache");

        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

        resp->reserve_body_buffer(sz);

        stream.read(resp->body_write_ptr(), static_cast<std::streamsize>(sz));
        resp->commit_body_bytes(static_cast<size_t>(stream.gcount()));
        resp->send();
    }

    void HttpServer::serve_static(HttpRequest * req, HttpResponse * resp)
    {
        const std::string &url = req->get_raw_url();
        std::string prefix;
        std::string path_prefix;
        std::string file_relative_path;
        if (!resolve_static_request(url, prefix, path_prefix, file_relative_path, resp)) {
            return;
        }

        if (file_relative_path.empty()) {
            serve_list_files(path_prefix, path_prefix, resp);
            return;
        }

        if (serve_embedded_static_page(file_relative_path, resp)) {
            return;
        }

        serve_static_file(req, resp, url, file_relative_path, path_prefix);
    }

    void HttpServer::serve_download(const std::string & filePath, const std::string & ext, HttpResponse * resp)
    {
        std::fstream file;
        file.open(std::filesystem::path(std::u8string(filePath.begin(), filePath.end())), std::ios::in | std::ios::binary);
        if (!file.good()) {
            resp->process_error(ResponseCode::not_found);
            return;
        }

        file.seekg(0, std::ios_base::end);
        std::size_t sz = file.tellg();
        file.close();

        const auto task = new net::http::HttpUploadFileTask([resp, filePath]() {
            LOG_INFO("Download completed: {}", filePath);
            resp->set_upload_file(false);
        });

        const auto attachment_info = std::make_shared<net::http::AttachmentInfo>();
        attachment_info->origin_file_name_ = filePath;
        attachment_info->length_ = sz;
        task->set_attachment_info(attachment_info);

        if (!task->init()) {
            resp->process_error(ResponseCode::internal_server_error);
            delete task;
            return;
        }

        resp->add_header("Content-Type", get_content_type(ext));
        resp->add_header("Connection", "close");
        resp->set_response_code(ResponseCode::ok_);
        resp->add_header("Content-Length", std::to_string(sz));

        resp->set_task(task);
        resp->set_upload_file(true);
        resp->send();
    }

    void HttpServer::serve_list_files(const std::string & prefix, const std::string & filePath, HttpResponse * resp)
    {
        try
        {
            std::filesystem::path absPrefix = std::filesystem::canonical(std::u8string(prefix.begin(), prefix.end()));
            std::filesystem::path absFile = std::filesystem::canonical(std::u8string(filePath.begin(), filePath.end()));

            auto relPath = std::filesystem::relative(absFile, absPrefix);
            std::string relStr = relPath.string();
            if (relStr.empty() || (relStr.size() >= 2 && relStr[0] == '.' && relStr[1] == '.')) {
                resp->process_error(ResponseCode::forbidden);
                return;
            }
        }
        catch (...)
        {
            resp->process_error(ResponseCode::internal_server_error);
            return;
        }

        nlohmann::json jsonResponse;
        const std::string dir = filePath.substr(prefix.length());

        try
        {
            for (const auto &entry : std::filesystem::directory_iterator(
                     std::filesystem::path(std::u8string(filePath.begin(), filePath.end())))) {

                nlohmann::json item;

                auto ftime = entry.last_write_time();
#if __cpp_lib_filesystem >= 201703L && __cplusplus > 201703L
                using namespace std::chrono;
                const auto sys_time = time_point_cast<system_clock::duration>(ftime - file_clock::now() + system_clock::now());
#else
                const auto sys_time = std::chrono::system_clock::now();
#endif

                if (entry.is_regular_file()) {
                    item["type"] = 1;
                    item["name"] = entry.path().filename().string();
                    item["size"] = std::filesystem::file_size(entry);
                    item["modified"] = std::chrono::system_clock::to_time_t(sys_time);
                    item["url"] = dir + entry.path().filename().string();
                } else if (entry.is_directory()) {
                    item["name"] = entry.path().filename().string();
                    item["type"] = 2;
                    item["modified"] = std::chrono::system_clock::to_time_t(sys_time);
                    item["url"] = dir + entry.path().filename().string() + "/";
                }

                jsonResponse.push_back(item);
            }
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            resp->process_error();
            return;
        }

        resp->json(jsonResponse.dump());
        resp->add_header("Connection", "close");
        resp->send();
    }

    void HttpServer::reload_config(HttpRequest * req, HttpResponse * resp)
    {
        (void)req;
        if (HttpConfigManager::get_instance()->reload_config()) {
            load_static_paths();
            resp->append_body("Configuration reloaded successfully.");
        } else {
            resp->append_body("Failed to reload configuration.");
        }

        resp->add_header("Content-Type", "text/plain; charset=utf-8");
        resp->add_header("Connection", "close");
        resp->set_response_code(ResponseCode::ok_);
        resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
        resp->send();
    }

    void HttpServer::handle_options_preflight(HttpRequest * req, HttpResponse * resp)
    {
        (void)req;
        resp->set_response_code(ResponseCode::no_content);

        if (config_.enable_cors) {
            resp->add_header("Access-Control-Allow-Origin", "*");
            resp->add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, PATCH");
            resp->add_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
            resp->add_header("Access-Control-Max-Age", "86400");
        }

        resp->add_header("Content-Length", "0");
        resp->send();
    }

    bool HttpServer::parse_upload_request(
        HttpRequest * req,
        HttpResponse * resp,
        FormDataContent * &form,
        std::string & upload_id,
        int & chunk_index,
        std::string & filename,
        FormDataFileItem * &file_item,
        uint64_t & chunk_size,
        int & total_chunks,
        uint64_t & file_size)
    {
        if (req->get_method() != HttpMethod::post_) {
            resp->process_error(ResponseCode::method_not_allowed);
            return false;
        }

        const auto &content = req->get_body_content();
        if (!content || !content->is_valid()) {
            nlohmann::json err;
            err["error"] = "no content";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        form = content->as<FormDataContent>();
        if (!form) {
            nlohmann::json err;
            err["error"] = "not multipart form-data, type=" + std::to_string(static_cast<int>(content->type));
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        upload_id = form->get_string("uploadid");
        if (upload_id.empty()) {
            nlohmann::json err;
            err["error"] = "missing uploadid";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        try
        {
            chunk_index = std::stoi(form->get_string("chunkindex"));
        }
        catch (...)
        {
            chunk_index = -1;
        }
        if (chunk_index < 0) {
            nlohmann::json err;
            err["error"] = "invalid chunkindex";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        filename = form->get_string("filename");
        if (chunk_index == 0 && filename.empty()) {
            nlohmann::json err;
            err["error"] = "missing filename for first chunk";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }
        if (!filename.empty()) {
            filename = filename.substr(filename.find_last_of("/\\") + 1);
            if (filename.find("..") != std::string::npos) {
                filename.clear();
            }
        }

        file_item = form->get_file("file");
        if (!file_item) {
            nlohmann::json err;
            err["error"] = "missing file data";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        chunk_size = 0;
        if (file_item->is_in_memory()) {
            chunk_size = static_cast<uint64_t>(file_item->size());
        } else if (!file_item->tmp_file.empty()) {
            std::error_code ec;
            chunk_size = std::filesystem::file_size(std::filesystem::u8path(file_item->tmp_file), ec);
            if (ec) {
                chunk_size = 0;
            }
        }
        if (chunk_size == 0) {
            nlohmann::json err;
            err["error"] = "missing or empty file data";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        total_chunks = 0;
        file_size = 0;
        if (!form->get_string("totalchunks").empty()) {
            total_chunks = std::atoi(form->get_string("totalchunks").c_str());
        }
        if (!form->get_string("filesize").empty()) {
            file_size = static_cast<uint64_t>(std::atoll(form->get_string("filesize").c_str()));
        }

        return true;
    }

    bool HttpServer::find_or_create_upload_session(
        const std::string & upload_id,
        int chunk_index,
        const std::string & filename,
        int total_chunks,
        uint64_t file_size,
        HttpResponse * resp,
        std::unordered_map<std::string, UploadFileMapping>::iterator & session_it)
    {
        session_it = uploaded_chunks_.find(upload_id);
        if (session_it == uploaded_chunks_.end()) {
            UploadSession session;
            session.filename = filename.empty() ? "unknown" : filename;
            session.upload_id = upload_id;
            session.total_chunks = total_chunks > 0 ? total_chunks : 1;
            session.total_size = file_size;
            uploaded_chunks_[upload_id] = std::move(session);
            session_it = uploaded_chunks_.find(upload_id);
            return true;
        }

        if (session_it->second.received.contains(chunk_index)) {
            nlohmann::json ok;
            ok["uploaded"] = true;
            ok["chunkIdx"] = chunk_index;
            resp->json(ok.dump(), ResponseCode::ok_);
            resp->send();
            return false;
        }

        if (!filename.empty() && session_it->second.filename == "unknown") {
            session_it->second.filename = filename;
        }
        if (total_chunks > 0 && session_it->second.total_chunks == 0) {
            session_it->second.total_chunks = total_chunks;
        }
        if (file_size > 0 && session_it->second.total_size == 0) {
            session_it->second.total_size = file_size;
        }
        return true;
    }

    bool HttpServer::store_upload_chunk(
        HttpRequest * req,
        HttpResponse * resp,
        const std::string & upload_id,
        int chunk_index,
        FormDataFileItem * file_item,
        uint64_t chunk_size,
        UploadSession & session,
        UploadSession & session_snapshot,
        int & received_count)
    {
        (void)req;
        if (session.total_chunks > 0 && chunk_index >= session.total_chunks) {
            nlohmann::json err;
            err["error"] = "chunkindex out of range";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        constexpr uint64_t MAX_CHUNK_SIZE = 100 * 1024 * 1024;
        if (chunk_size > MAX_CHUNK_SIZE) {
            nlohmann::json err;
            err["error"] = "chunk too large";
            resp->json(err.dump(), ResponseCode::payload_too_large);
            resp->send();
            return false;
        }

        const bool is_last_chunk = session.total_chunks > 0 && chunk_index == session.total_chunks - 1;
        if (is_last_chunk && session.total_size > 0 && session.received_bytes() + chunk_size != session.total_size) {
            nlohmann::json err;
            err["error"] = "total size mismatch";
            resp->json(err.dump(), ResponseCode::bad_request);
            resp->send();
            return false;
        }

        UploadedChunk chunk;
        chunk.index = chunk_index;
        chunk.size = chunk_size;
        chunk.tmp_path = ".upload_tmp/" + upload_id + "_part" + std::to_string(chunk_index);
        session.received[chunk_index] = chunk;

        LOG_INFO("[Upload] chunk={}/{} size={} file={}", chunk_index, session.total_chunks, chunk_size, session.filename);

        std::error_code create_dir_ec;
        std::filesystem::create_directories(".upload_tmp", create_dir_ec);

        if (!file_item->is_in_memory()) {
            std::error_code copy_ec;
            std::filesystem::copy_file(
                std::filesystem::u8path(file_item->tmp_file),
                std::filesystem::u8path(chunk.tmp_path),
                std::filesystem::copy_options::overwrite_existing,
                copy_ec);
            if (copy_ec) {
                session.received.erase(chunk_index);
                nlohmann::json err;
                err["error"] = "failed to persist upload chunk";
                resp->json(err.dump(), ResponseCode::internal_server_error);
                resp->send();
                return false;
            }
        }

        received_count = static_cast<int>(session.received.size());
        session_snapshot = session;
        return true;
    }

    void HttpServer::finalize_upload_chunk(
        HttpRequest * req,
        int chunk_index,
        FormDataFileItem * file_item,
        const UploadSession & session_snapshot,
        int received_count)
    {
        const bool is_complete = session_snapshot.total_chunks > 0 && received_count == session_snapshot.total_chunks;

        if (file_item->is_in_memory()) {
            UploadTmpChunk tmp_chunk;
            tmp_chunk.chunk_ = session_snapshot.received.at(chunk_index);
            tmp_chunk.raw_buffer = req->get_context()->get_connection()->take_input_byte_buffer();
            if (file_item->data_begin && file_item->data_end > file_item->data_begin) {
                tmp_chunk.data_.assign(file_item->data_begin, file_item->data_end);
                tmp_chunk.begin_ = tmp_chunk.data_.data();
                tmp_chunk.end_ = tmp_chunk.begin_ + tmp_chunk.data_.size();
            }

            auto task = std::make_unique<SaveUploadTempChunkTask>(std::move(tmp_chunk));
            if (is_complete) {
                task->set_session(std::make_shared<UploadSession>(session_snapshot));
                uploaded_chunks_.erase(session_snapshot.upload_id);
                LOG_INFO("[Upload] complete: {} size={}", session_snapshot.filename, session_snapshot.total_size);
            }
            thread_pool_->push_task(std::move(task));
            return;
        }

        if (is_complete) {
            auto task = std::make_unique<SaveUploadTempChunkTask>();
            task->set_session(std::make_shared<UploadSession>(session_snapshot));
            uploaded_chunks_.erase(session_snapshot.upload_id);
            thread_pool_->push_task(std::move(task));
            LOG_INFO("[Upload] complete: {} size={}", session_snapshot.filename, session_snapshot.total_size);
        }
    }

    void HttpServer::serve_upload(HttpRequest * req, HttpResponse * resp)
    {
        FormDataContent *form = nullptr;
        FormDataFileItem *file_item = nullptr;
        std::string upload_id;
        std::string filename;
        int chunk_index = 0;
        uint64_t chunk_size = 0;
        int total_chunks = 0;
        uint64_t file_size = 0;

        if (!parse_upload_request(
                 req,
                 resp,
                 form,
                 upload_id,
                 chunk_index,
                 filename,
                 file_item,
                 chunk_size,
                 total_chunks,
                 file_size)) {
            return;
        }

        std::unordered_map<std::string, UploadFileMapping>::iterator session_it;
        if (!find_or_create_upload_session(
                 upload_id,
                 chunk_index,
                 filename,
                 total_chunks,
                 file_size,
                 resp,
                 session_it)) {
            return;
        }

        UploadSession session_snapshot;
        int received_count = 0;
        if (!store_upload_chunk(
                 req,
                 resp,
                 upload_id,
                 chunk_index,
                 file_item,
                 chunk_size,
                 session_it->second,
                 session_snapshot,
                 received_count)) {
            return;
        }

        finalize_upload_chunk(req, chunk_index, file_item, session_snapshot, received_count);

        nlohmann::json result;
        result["uploaded"] = true;
        result["chunkIdx"] = chunk_index;
        result["received"] = received_count;
        result["total"] = session_snapshot.total_chunks;
        resp->json(result.dump(), ResponseCode::ok_);
        resp->send();
    }
}
