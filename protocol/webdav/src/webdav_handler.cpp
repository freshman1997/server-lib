#include "webdav_handler.h"

#include "context.h"
#include "http_server.h"
#include "request.h"
#include "response.h"
#include "response_code.h"
#include "task/upload_file_task.h"
#include "webdav_xml.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>

namespace yuan::net::webdav
{
    namespace
    {
        std::string trim_slash(std::string path)
        {
            if (path.empty() || path.front() != '/') {
                path.insert(path.begin(), '/');
            }
            while (path.size() > 1 && path.back() == '/') {
                path.pop_back();
            }
            return path;
        }

        http::ResponseCode code_of(int status)
        {
            return static_cast<http::ResponseCode>(status);
        }

        std::string request_body(http::HttpRequest *req)
        {
            if (!req) {
                return {};
            }
            if (req->body_buffer_size() > 0) {
                return req->body_buffer_text();
            }
            const auto len = req->get_body_length();
            const char *begin = req->body_begin();
            const char *end = req->body_end();
            if (len == 0 || !begin || !end || end < begin) {
                return {};
            }
            return std::string(begin, end);
        }

        std::vector<Property> parse_proppatch_values(std::string_view body)
        {
            std::vector<Property> props;
            std::size_t pos = 0;
            while ((pos = body.find("<D:", pos)) != std::string_view::npos) {
                const auto name_start = pos + 3;
                const auto name_end = body.find_first_of(" >", name_start);
                if (name_end == std::string_view::npos) {
                    break;
                }
                const std::string name(body.substr(name_start, name_end - name_start));
                if (name == "propertyupdate" || name == "set" || name == "prop" || name == "remove") {
                    pos = name_end;
                    continue;
                }
                const std::string close = "</D:" + name + ">";
                const auto value_start = body.find('>', name_end);
                const auto value_end = value_start == std::string_view::npos ? std::string_view::npos : body.find(close, value_start);
                if (value_start != std::string_view::npos && value_end != std::string_view::npos) {
                    props.push_back(Property{ "DAV:", name, std::string(body.substr(value_start + 1, value_end - value_start - 1)) });
                    pos = value_end + close.size();
                } else {
                    pos = name_end;
                }
            }
            return props;
        }

        std::chrono::seconds parse_lock_timeout(http::HttpRequest *req,
                                                std::uint32_t default_seconds,
                                                std::uint32_t max_seconds)
        {
            const auto fallback = std::chrono::seconds(default_seconds > 0 ? default_seconds : 3600);
            if (!req) {
                return fallback;
            }

            const auto *timeout_header = req->get_header("timeout");
            if (!timeout_header || timeout_header->empty()) {
                return fallback;
            }

            std::string value = *timeout_header;
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
            if (value.rfind("SECOND-", 0) != 0) {
                return fallback;
            }

            const auto sec_text = value.substr(7);
            if (sec_text.empty()) {
                return fallback;
            }
            std::uint64_t seconds = 0;
            try {
                seconds = static_cast<std::uint64_t>(std::stoull(sec_text));
            } catch (...) {
                return fallback;
            }

            const std::uint64_t capped_max = max_seconds > 0 ? max_seconds : 86400;
            if (seconds == 0) {
                seconds = default_seconds > 0 ? default_seconds : 3600;
            }
            if (seconds > capped_max) {
                seconds = capped_max;
            }
            return std::chrono::seconds(seconds);
        }
    }

    WebDavHandler::WebDavHandler(std::shared_ptr<WebDavResourceBackend> backend, WebDavHandlerConfig config)
        : backend_(std::move(backend)), config_(std::move(config)), locks_(std::make_shared<WebDavLockManager>())
    {
        config_.mount_path = trim_slash(config_.mount_path);
    }

    WebDavHandler::WebDavHandler(std::shared_ptr<WebDavResourceBackend> backend,
                                 std::shared_ptr<WebDavLockManager> locks,
                                 WebDavHandlerConfig config)
        : backend_(std::move(backend)),
          config_(std::move(config)),
          locks_(locks ? std::move(locks) : std::make_shared<WebDavLockManager>())
    {
        config_.mount_path = trim_slash(config_.mount_path);
    }

    void WebDavHandler::handle(http::HttpRequest *req, http::HttpResponse *resp)
    {
        if (!req || !resp || !backend_) {
            return;
        }

        switch (req->get_method()) {
        case http::HttpMethod::options_: options(req, resp); break;
        case http::HttpMethod::get_: get_or_head(req, resp, false); break;
        case http::HttpMethod::head_: get_or_head(req, resp, true); break;
        case http::HttpMethod::put_: put(req, resp); break;
        case http::HttpMethod::delete_: remove(req, resp); break;
        case http::HttpMethod::mkcol_: mkcol(req, resp); break;
        case http::HttpMethod::propfind_: propfind(req, resp); break;
        case http::HttpMethod::proppatch_: proppatch(req, resp); break;
        case http::HttpMethod::copy_: copy_or_move(req, resp, false); break;
        case http::HttpMethod::move_: copy_or_move(req, resp, true); break;
        case http::HttpMethod::lock_: lock(req, resp); break;
        case http::HttpMethod::unlock_: unlock(req, resp); break;
        case http::HttpMethod::report_:
        case http::HttpMethod::search_: report_or_search(req, resp); break;
        case http::HttpMethod::acl_:
            resp->set_response_code(http::ResponseCode::not_implemented);
            finish(resp);
            break;
        default:
            resp->set_response_code(http::ResponseCode::method_not_allowed);
            resp->add_header("Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK, REPORT, SEARCH");
            finish(resp);
            break;
        }
    }

    http::request_function WebDavHandler::as_handler()
    {
        return [this](http::HttpRequest *req, http::HttpResponse *resp) {
            handle(req, resp);
        };
    }

    std::string WebDavHandler::href_from_request(const http::HttpRequest &req) const
    {
        std::string path(req.get_path());
        if (config_.mount_path != "/" && path.rfind(config_.mount_path, 0) == 0) {
            path.erase(0, config_.mount_path.size());
        }
        if (path.empty()) {
            path = "/";
        }
        return trim_slash(path);
    }

    std::string WebDavHandler::destination_from_request(const http::HttpRequest &req) const
    {
        const auto *dest = req.get_header("destination");
        if (!dest || dest->empty()) {
            return {};
        }
        std::string path = *dest;
        const auto scheme = path.find("://");
        if (scheme != std::string::npos) {
            const auto slash = path.find('/', scheme + 3);
            path = slash == std::string::npos ? "/" : path.substr(slash);
        }
        if (config_.mount_path != "/" && path.rfind(config_.mount_path, 0) == 0) {
            path.erase(0, config_.mount_path.size());
        }
        return trim_slash(path);
    }

    bool WebDavHandler::check_write_lock(const std::string &href, const http::HttpRequest &req, http::HttpResponse *resp) const
    {
        const auto *ifh = req.get_header("if");
        const auto *lock_token = req.get_header("lock-token");
        const std::string token = ifh ? *ifh : (lock_token ? *lock_token : "");
        if (locks_->allows(href, token)) {
            return true;
        }
        resp->set_response_code(http::ResponseCode::locked);
        finish(resp);
        return false;
    }

    void WebDavHandler::finish(http::HttpResponse *resp) const
    {
        if (!resp->get_header("Content-Length")) {
            resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
        }
        if (resp->get_context() && resp->get_context()->get_connection()) {
            resp->send();
        }
    }

    void WebDavHandler::options(http::HttpRequest *, http::HttpResponse *resp)
    {
        resp->set_response_code(http::ResponseCode::no_content);
        resp->add_header("DAV", "1, 2, 3");
        resp->add_header("MS-Author-Via", "DAV");
        resp->add_header("Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK, REPORT, SEARCH");
        finish(resp);
    }

    void WebDavHandler::get_or_head(http::HttpRequest *req, http::HttpResponse *resp, bool head)
    {
        const auto href = href_from_request(*req);
        const auto info = backend_->stat(href);
        if (!info.exists) {
            resp->set_response_code(http::ResponseCode::not_found);
            finish(resp);
            return;
        }
        if (info.is_collection) {
            req->add_header("Depth", "1");
            propfind(req, resp);
            return;
        }
        if (!head) {
            if (const auto path = backend_->file_path_for_read(href)) {
                auto task = std::make_unique<http::HttpUploadFileTask>([resp]() {
                    resp->set_upload_file(false);
                });
                auto attachment = std::make_shared<http::AttachmentInfo>();
                attachment->origin_file_name_ = path->string();
                attachment->length_ = static_cast<std::size_t>(info.content_length);
                task->set_attachment_info(std::move(attachment));
                if (task->init()) {
                    resp->set_response_code(http::ResponseCode::ok_);
                    resp->add_header("Content-Type", info.content_type);
                    resp->add_header("ETag", info.etag);
                    resp->add_header("Last-Modified", format_http_date(info.last_modified));
                    resp->add_header("Content-Length", std::to_string(info.content_length));
                    resp->set_task(task.release());
                    resp->set_upload_file(true);
                    resp->send();
                    return;
                }
            }
        }
        const auto body = backend_->read(href);
        if (!body) {
            resp->set_response_code(http::ResponseCode::not_found);
            finish(resp);
            return;
        }
        resp->set_response_code(http::ResponseCode::ok_);
        resp->add_header("Content-Type", info.content_type);
        resp->add_header("ETag", info.etag);
        resp->add_header("Last-Modified", format_http_date(info.last_modified));
        if (!head) {
            resp->append_body(*body);
        }
        resp->add_header("Content-Length", std::to_string(head ? info.content_length : resp->body_buffer_size()));
        finish(resp);
    }

    void WebDavHandler::put(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto href = href_from_request(*req);
        if (!check_write_lock(href, *req, resp)) {
            return;
        }
        if (req->has_body_file()) {
            const auto result = backend_->write_from_file(href, req->body_file_path(), config_.create_parent_on_put);
            resp->set_response_code(code_of(result.status));
            finish(resp);
            return;
        }
        const std::string body = request_body(req);
        if (body.size() > config_.max_put_bytes) {
            resp->set_response_code(http::ResponseCode::payload_too_large);
            finish(resp);
            return;
        }
        const auto result = backend_->write(href, body, config_.create_parent_on_put);
        resp->set_response_code(code_of(result.status));
        finish(resp);
    }

    void WebDavHandler::remove(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto href = href_from_request(*req);
        if (!check_write_lock(href, *req, resp)) {
            return;
        }
        const auto result = backend_->remove(href);
        resp->set_response_code(code_of(result.status));
        finish(resp);
    }

    void WebDavHandler::mkcol(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto href = href_from_request(*req);
        if (!check_write_lock(href, *req, resp)) {
            return;
        }
        if (req->get_body_length() > 0) {
            resp->set_response_code(http::ResponseCode::unsupported_media_type);
            finish(resp);
            return;
        }
        const auto result = backend_->make_collection(href);
        resp->set_response_code(code_of(result.status));
        finish(resp);
    }

    void WebDavHandler::propfind(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto href = href_from_request(*req);
        const auto depth = parse_depth(req->get_header("depth") ? *req->get_header("depth") : "infinity", Depth::infinity);
        if (depth == Depth::infinity && !config_.allow_infinite_depth) {
            resp->set_response_code(http::ResponseCode::forbidden);
            finish(resp);
            return;
        }
        const auto info = backend_->stat(href);
        if (!info.exists) {
            resp->set_response_code(http::ResponseCode::not_found);
            finish(resp);
            return;
        }
        const auto append_response = [&](std::string &xml, const std::string &child_href, const ResourceInfo &child_info) {
            xml += propfind_response_xml(child_href, child_info, backend_->dead_properties(child_href),
                                         backend_->quota(child_href), locks_->active_locks(child_href));
        };
        std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
        append_response(xml, href, info);
        if (depth != Depth::zero && info.is_collection) {
            for (const auto &child : backend_->list(href)) {
                std::string child_href = trim_slash(href + "/" + child.name);
                if (child.info.is_collection) {
                    child_href += "/";
                }
                append_response(xml, child_href, child.info);
            }
        }
        xml += "</D:multistatus>";
        resp->set_response_code(http::ResponseCode::multi_status);
        resp->add_header("Content-Type", "application/xml; charset=utf-8");
        resp->append_body(xml);
        finish(resp);
    }

    void WebDavHandler::proppatch(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto href = href_from_request(*req);
        if (!check_write_lock(href, *req, resp)) {
            return;
        }
        const auto props = parse_proppatch_values(request_body(req));
        const auto result = backend_->set_properties(href, props);
        resp->set_response_code(result.ok ? http::ResponseCode::multi_status : code_of(result.status));
        resp->add_header("Content-Type", "application/xml; charset=utf-8");
        PropertyResult pr;
        pr.href = href;
        pr.status = result.ok ? 200 : result.status;
        pr.properties = props;
        resp->append_body(multistatus_xml({ pr }));
        finish(resp);
    }

    void WebDavHandler::copy_or_move(http::HttpRequest *req, http::HttpResponse *resp, bool move)
    {
        const auto href = href_from_request(*req);
        const auto dest = destination_from_request(*req);
        if (dest.empty()) {
            resp->set_response_code(http::ResponseCode::bad_request);
            finish(resp);
            return;
        }
        if (!check_write_lock(href, *req, resp) || !check_write_lock(dest, *req, resp)) {
            return;
        }
        const bool overwrite = parse_overwrite(req->get_header("overwrite") ? *req->get_header("overwrite") : "T");
        const auto result = move ? backend_->move(href, dest, overwrite) : backend_->copy(href, dest, overwrite);
        resp->set_response_code(code_of(result.status));
        finish(resp);
    }

    void WebDavHandler::lock(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto href = href_from_request(*req);
        const auto timeout = parse_lock_timeout(req,
                                                config_.default_lock_timeout_seconds,
                                                config_.max_lock_timeout_seconds);
        const auto *lock_token = req->get_header("if");
        if (lock_token && locks_->refresh(*lock_token, timeout)) {
            const auto refreshed = locks_->find(*lock_token);
            resp->set_response_code(http::ResponseCode::ok_);
            resp->add_header("Content-Type", "application/xml; charset=utf-8");
            resp->append_body("<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery>" +
                              lockdiscovery_xml(refreshed ? std::vector<LockInfo>{ *refreshed } : std::vector<LockInfo>{}) +
                              "</D:lockdiscovery></D:prop>");
            finish(resp);
            return;
        }
        if (!locks_->allows(href, "")) {
            resp->set_response_code(http::ResponseCode::locked);
            finish(resp);
            return;
        }
        const auto depth = parse_depth(req->get_header("depth") ? *req->get_header("depth") : "infinity", Depth::infinity);
        auto lock_info = locks_->create(href, parse_lock_scope(request_body(req)), depth, parse_lock_owner(request_body(req)),
                                        timeout);
        resp->set_response_code(http::ResponseCode::ok_);
        resp->add_header("Lock-Token", "<" + lock_info.token + ">");
        resp->add_header("Content-Type", "application/xml; charset=utf-8");
        resp->append_body("<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery>" +
                          lockdiscovery_xml({ lock_info }) + "</D:lockdiscovery></D:prop>");
        finish(resp);
    }

    void WebDavHandler::unlock(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto *token = req->get_header("lock-token");
        if (!token || !locks_->unlock(*token)) {
            resp->set_response_code(http::ResponseCode::precondition_failed);
            finish(resp);
            return;
        }
        resp->set_response_code(http::ResponseCode::no_content);
        finish(resp);
    }

    void WebDavHandler::report_or_search(http::HttpRequest *req, http::HttpResponse *resp)
    {
        const auto href = href_from_request(*req);
        const auto info = backend_->stat(href);
        if (!info.exists) {
            resp->set_response_code(http::ResponseCode::not_found);
            finish(resp);
            return;
        }
        propfind(req, resp);
    }

    void mount_webdav(http::HttpServer &server,
                      const std::string &mount_path,
                      std::shared_ptr<WebDavResourceBackend> backend,
                      WebDavHandlerConfig config)
    {
        config.mount_path = mount_path;
        auto handler = std::make_shared<WebDavHandler>(std::move(backend), std::move(config));
        server.on(mount_path, [handler](http::HttpRequest *req, http::HttpResponse *resp) {
            handler->handle(req, resp);
        }, true);
    }
}
