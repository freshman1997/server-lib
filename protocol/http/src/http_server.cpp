#include "content/types.h"
#include "context.h"
#include "coroutine/io_result.h"
#include "net/runtime/network_runtime.h"
#include "net/security/openssl.h"
#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"
#include "task/save_upload_tmp_chunk_task.h"
#include "task/upload_file_task.h"
#include "url.h"
#include "base/time.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "logger.h"
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <functional>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <winnls.h>
#include <winnt.h>
#else
#include <unistd.h>
#endif

#ifdef YUAN_HTTP_HAS_ZLIB
#include <zlib.h>
#endif

#if YUAN_HTTP_HAS_BROTLI
#include <brotli/encode.h>
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
#include "proxy_api.h"
#include "header_key.h"
#include "header_util.h"
#include "http2/session.h"

namespace yuan::net::http
{
    namespace
    {
        constexpr uint64_t kUploadCleanupIntervalMs = 30000;
        constexpr uint64_t kUploadSessionTtlMs = 10 * 60 * 1000;
        constexpr uint64_t kUploadTmpFileTtlMs = 30 * 60 * 1000;
        constexpr std::size_t kStaticCompressionBufferLimit = 2 * 1024 * 1024;

        bool parse_upload_tmp_upload_id(const std::string &file_name, std::string &upload_id)
        {
            const std::string marker = "_part";
            const std::size_t marker_pos = file_name.find(marker);
            if (marker_pos == std::string::npos || marker_pos == 0 || marker_pos + marker.size() >= file_name.size()) {
                return false;
            }

            const std::string part_idx = file_name.substr(marker_pos + marker.size());
            if (part_idx.empty() || !std::all_of(part_idx.begin(), part_idx.end(), [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                })) {
                return false;
            }

            upload_id.assign(file_name.data(), marker_pos);
            return !upload_id.empty();
        }

        bool is_safe_upload_id(const std::string &upload_id)
        {
            if (upload_id.empty() || upload_id.size() > 128) {
                return false;
            }
            return std::all_of(upload_id.begin(), upload_id.end(), [](unsigned char ch) {
                return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
            });
        }

        uint64_t file_age_ms(const std::filesystem::path &path)
        {
            std::error_code ec;
            const auto write_time = std::filesystem::last_write_time(path, ec);
            if (ec) {
                return 0;
            }

            const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::filesystem::file_time_type::clock::now() - write_time)
                                    .count();
            if (age_ms <= 0) {
                return 0;
            }
            return static_cast<uint64_t>(age_ms);
        }

        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

        bool iequals_ascii(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                const unsigned char lc = static_cast<unsigned char>(lhs[i]);
                const unsigned char rc = static_cast<unsigned char>(rhs[i]);
                if (std::tolower(lc) != std::tolower(rc)) {
                    return false;
                }
            }
            return true;
        }

        bool is_textual_content_type(const std::string &content_type)
        {
            return content_type.rfind("text/", 0) == 0 ||
                   content_type.find("json") != std::string::npos ||
                   content_type.find("xml") != std::string::npos ||
                   content_type.find("javascript") != std::string::npos;
        }

        std::optional<std::string> parse_accept_encoding_preferred(std::string_view value)
        {
            std::optional<double> best_q;
            std::optional<std::string> best_encoding;

            std::size_t pos = 0;
            while (pos < value.size()) {
                std::size_t comma = value.find(',', pos);
                if (comma == std::string_view::npos) {
                    comma = value.size();
                }

                std::string_view token = value.substr(pos, comma - pos);
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
                    token.remove_prefix(1);
                }
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
                    token.remove_suffix(1);
                }

                double q = 1.0;
                std::size_t semicolon = token.find(';');
                std::string_view encoding = semicolon == std::string_view::npos ? token : token.substr(0, semicolon);
                while (!encoding.empty() && std::isspace(static_cast<unsigned char>(encoding.back()))) {
                    encoding.remove_suffix(1);
                }

                if (semicolon != std::string_view::npos) {
                    std::string_view params = token.substr(semicolon + 1);
                    const std::size_t qpos = params.find("q=");
                    if (qpos != std::string_view::npos) {
                        const std::string_view qv = params.substr(qpos + 2);
                        const std::string qstr(qv.begin(), qv.end());
                        try {
                            q = std::stod(qstr);
                        } catch (...) {
                            q = 0.0;
                        }
                    }
                }

                if (!encoding.empty() && q > 0.0) {
                    if (iequals_ascii(encoding, "br") || iequals_ascii(encoding, "gzip")) {
                        const bool prefer = !best_q || q > *best_q || (q == *best_q && iequals_ascii(encoding, "br"));
                        if (prefer) {
                            best_q = q;
                            best_encoding = std::string(encoding);
                        }
                    }
                }

                pos = comma + 1;
            }

            return best_encoding;
        }

        std::string read_file_to_string(const std::string &path)
        {
            std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
            if (!in.good()) {
                return {};
            }
            std::ostringstream oss;
            oss << in.rdbuf();
            return oss.str();
        }

        std::string_view trim_ascii(std::string_view sv)
        {
            std::size_t begin = 0;
            while (begin < sv.size() && std::isspace(static_cast<unsigned char>(sv[begin]))) {
                ++begin;
            }
            std::size_t end = sv.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
                --end;
            }
            return sv.substr(begin, end - begin);
        }

        bool header_has_token(const std::string *value, std::string_view token)
        {
            if (!value) {
                return false;
            }

            std::size_t pos = 0;
            while (pos <= value->size()) {
                const std::size_t comma = value->find(',', pos);
                const std::size_t end = comma == std::string::npos ? value->size() : comma;
                if (iequals_ascii(trim_ascii(std::string_view(*value).substr(pos, end - pos)), token)) {
                    return true;
                }
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
            return false;
        }

        uint32_t keep_alive_idle_timeout_ms()
        {
            return config::connection_idle_timeout > 0
                ? static_cast<uint32_t>(config::connection_idle_timeout)
                : uint32_t{30000};
        }

        uint32_t response_write_timeout_ms(const HttpServerConfig &config)
        {
            return config.write_timeout_ms > 0
                ? static_cast<uint32_t>(config.write_timeout_ms)
                : keep_alive_idle_timeout_ms();
        }

        bool should_close_http1_connection(HttpRequest *req, HttpResponse *resp, bool keep_alive_enabled)
        {
            if (!req || !resp) {
                return true;
            }
            if (resp->is_sse()) {
                return false;
            }
            if (!keep_alive_enabled) {
                return true;
            }
            if (header_has_token(resp->get_header(http_header_key::connection), "close") ||
                req->connection_close_requested()) {
                return true;
            }
            if (req->get_version() == HttpVersion::v_1_0 &&
                !req->connection_keep_alive_requested()) {
                return true;
            }
            return false;
        }

        void apply_http1_connection_response_headers(HttpRequest *req, HttpResponse *resp, bool keep_alive_enabled)
        {
            if (!req || !resp || resp->is_sse()) {
                return;
            }

            if (!keep_alive_enabled) {
                resp->add_header("Connection", "close");
                return;
            }

            if (req->get_version() == HttpVersion::v_1_0) {
                if (req->connection_keep_alive_requested()) {
                    resp->add_header("Connection", "keep-alive");
                    resp->add_header("Keep-Alive", "timeout=60, max=1000");
                } else {
                    resp->add_header("Connection", "close");
                }
            }
        }

        constexpr std::string_view kHttp2ConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

        enum class Http2PrefaceProbe
        {
            need_more,
            is_preface,
            not_preface
        };

        Http2PrefaceProbe probe_http2_preface(const ::yuan::buffer::ByteBuffer &data)
        {
            const auto span = data.readable_span();
            if (span.empty()) {
                return Http2PrefaceProbe::need_more;
            }

            const std::string_view sample(span.data(), span.size());
            const std::size_t check_size = (std::min)(sample.size(), kHttp2ConnectionPreface.size());
            if (sample.substr(0, check_size) != kHttp2ConnectionPreface.substr(0, check_size)) {
                return Http2PrefaceProbe::not_preface;
            }

            if (sample.size() < kHttp2ConnectionPreface.size()) {
                return Http2PrefaceProbe::need_more;
            }

            return Http2PrefaceProbe::is_preface;
        }

        struct Http2AssembledStream
        {
            std::unordered_map<std::string, std::string> pseudo_headers;
            std::unordered_map<std::string, std::string> regular_headers;
            std::string body;
            bool headers_done = false;
            bool end_stream = false;
            bool trailers = false;
            bool body_oversized = false;
            std::unique_ptr<HttpSessionContext> stream_context;

            static constexpr std::size_t kMaxBodySize = 10 * 1024 * 1024;
        };

        bool validate_h2_pseudo_headers(const std::unordered_map<std::string, std::string> &pseudo_headers,
                                        std::string &method,
                                        std::string &path,
                                        std::string &authority)
        {
            const auto method_it = pseudo_headers.find(":method");
            const auto path_it = pseudo_headers.find(":path");

            if (method_it == pseudo_headers.end() || path_it == pseudo_headers.end()) {
                return false;
            }

            method = method_it->second;
            path = path_it->second;
            if (method.empty() || path.empty() || path.front() != '/') {
                return false;
            }

            const auto authority_it = pseudo_headers.find(":authority");
            authority = authority_it != pseudo_headers.end() ? authority_it->second : "";
            return true;
        }

        std::string trim_copy(std::string_view sv)
        {
            std::size_t b = 0;
            while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) {
                ++b;
            }
            std::size_t e = sv.size();
            while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) {
                --e;
            }
            return std::string(sv.substr(b, e - b));
        }

        void parse_h2_header_block(std::string_view raw,
                                   std::unordered_map<std::string, std::string> &pseudo_headers,
                                   std::unordered_map<std::string, std::string> &regular_headers)
        {
            pseudo_headers.clear();
            regular_headers.clear();
            bool seen_regular = false;
            std::unordered_set<std::string> seen_pseudo;

            std::size_t pos = 0;
            while (pos < raw.size()) {
                std::size_t nl = raw.find('\n', pos);
                if (nl == std::string_view::npos) {
                    nl = raw.size();
                }

                std::string_view line = raw.substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }

                std::size_t sep = std::string_view::npos;
                if (!line.empty() && line.front() == ':') {
                    sep = line.find(':', 1);
                } else {
                    sep = line.find(':');
                }

                if (sep != std::string_view::npos && sep + 1 <= line.size()) {
                    std::string key = trim_copy(line.substr(0, sep));
                    std::string val = trim_copy(line.substr(sep + 1));
                    if (!key.empty()) {
                        const bool is_pseudo = key.front() == ':';
                        if (is_pseudo) {
                            if (seen_regular) {
                                pseudo_headers.clear();
                                regular_headers.clear();
                                pseudo_headers[":__invalid"] = "pseudo-after-regular";
                                return;
                            }
                            if (key != ":method" && key != ":scheme" && key != ":authority" && key != ":path") {
                                pseudo_headers.clear();
                                regular_headers.clear();
                                pseudo_headers[":__invalid"] = "unknown-pseudo";
                                return;
                            }
                            if (!seen_pseudo.insert(key).second) {
                                pseudo_headers.clear();
                                regular_headers.clear();
                                pseudo_headers[":__invalid"] = "duplicate-pseudo";
                                return;
                            }
                            pseudo_headers[std::move(key)] = std::move(val);
                        } else {
                            seen_regular = true;
                            regular_headers[std::move(key)] = std::move(val);
                        }
                    }
                }

                pos = nl + 1;
            }
        }

        void maybe_log_h2_stream_complete(std::uint32_t stream_id, std::unordered_map<std::uint32_t, Http2AssembledStream> &streams)
        {
            auto it = streams.find(stream_id);
            if (it == streams.end()) {
                return;
            }

            auto &stream = it->second;
            if (!stream.headers_done || !stream.end_stream) {
                return;
            }

            const auto method_it = stream.pseudo_headers.find(":method");
            const auto path_it = stream.pseudo_headers.find(":path");
            const auto authority_it = stream.pseudo_headers.find(":authority");
            LOG_DEBUG("[HTTP2] assembled stream={} method={} path={} authority={} body_bytes={}",
                      stream_id,
                      method_it != stream.pseudo_headers.end() ? method_it->second : "",
                      path_it != stream.pseudo_headers.end() ? path_it->second : "",
                      authority_it != stream.pseudo_headers.end() ? authority_it->second : "",
                      stream.body.size());
        }

        bool maybe_reply_h2_via_dispatcher(std::uint32_t stream_id,
                                           std::unordered_map<std::uint32_t, Http2AssembledStream> &streams,
                                           const std::shared_ptr<http2::Session> &session,
                                           const std::shared_ptr<net::Connection> &conn,
                                           const std::function<bool(HttpSessionContext *)> &dispatch_fn)
        {
            if (!session || !conn || !dispatch_fn) {
                return false;
            }

            auto it = streams.find(stream_id);
            if (it == streams.end()) {
                return false;
            }

            auto &s = it->second;
            if (!s.headers_done || !s.end_stream) {
                return false;
            }

            if (s.body_oversized) {
                session->send_simple_response(stream_id, 413, "text/plain", "payload too large");
                streams.erase(it);
                return true;
            }

            if (!s.stream_context) {
                s.stream_context = std::make_unique<HttpSessionContext>(conn);
            }

            auto *req = s.stream_context->get_request();
            auto *resp = s.stream_context->get_response();
            if (!req || !resp) {
                streams.erase(it);
                return false;
            }

            req->reset();
            resp->reset();

            std::string method;
            std::string path;
            std::string authority;
            if (s.pseudo_headers.contains(":__invalid") ||
                !validate_h2_pseudo_headers(s.pseudo_headers, method, path, authority)) {
                session->send_simple_response(stream_id, 400, "text/plain", "invalid pseudo headers");
                streams.erase(it);
                return true;
            }

            if (method == "POST") {
                req->set_method(HttpMethod::post_);
            } else if (method == "PUT") {
                req->set_method(HttpMethod::put_);
            } else if (method == "DELETE") {
                req->set_method(HttpMethod::delete_);
            } else if (method == "OPTIONS") {
                req->set_method(HttpMethod::options_);
            } else if (method == "HEAD") {
                req->set_method(HttpMethod::head_);
            } else if (method == "PATCH") {
                req->set_method(HttpMethod::patch_);
            } else if (method == "PROPFIND") {
                req->set_method(HttpMethod::propfind_);
            } else if (method == "PROPPATCH") {
                req->set_method(HttpMethod::proppatch_);
            } else if (method == "MKCOL") {
                req->set_method(HttpMethod::mkcol_);
            } else if (method == "COPY") {
                req->set_method(HttpMethod::copy_);
            } else if (method == "MOVE") {
                req->set_method(HttpMethod::move_);
            } else if (method == "LOCK") {
                req->set_method(HttpMethod::lock_);
            } else if (method == "UNLOCK") {
                req->set_method(HttpMethod::unlock_);
            } else if (method == "REPORT") {
                req->set_method(HttpMethod::report_);
            } else if (method == "ACL") {
                req->set_method(HttpMethod::acl_);
            } else if (method == "SEARCH") {
                req->set_method(HttpMethod::search_);
            } else {
                req->set_method(HttpMethod::get_);
            }

            req->set_raw_url(path);
            req->set_version(HttpVersion::v_2_0);

            if (!authority.empty()) {
                req->add_header(http_header_key::host, authority);
            }

            for (const auto &[k, v] : s.regular_headers) {
                if (k == "host" || k == "content-length" || k == "transfer-encoding") {
                    continue;
                }
                req->add_header(k, v);
            }

            if (!s.body.empty()) {
                req->append_body(s.body);
                req->set_body_length(static_cast<std::uint32_t>(s.body.size()));
                req->add_header(http_header_key::content_length, std::to_string(s.body.size()));
            }

            (void)dispatch_fn(s.stream_context.get());

            std::uint16_t status = static_cast<std::uint16_t>(resp->get_response_code());
            if (status == 0 || status == static_cast<std::uint16_t>(ResponseCode::invalid)) {
                status = static_cast<std::uint16_t>(ResponseCode::ok_);
            }

            const auto *ct = resp->get_header(http_header_key::content_type);
            const std::string content_type = ct ? *ct : "application/octet-stream";

            const std::string status_str = std::to_string(status);
            std::vector<std::pair<std::string_view, std::string_view>> resp_headers;
            resp_headers.emplace_back(":status", status_str);
            resp_headers.emplace_back("content-type", content_type);

            const std::size_t body_sz = resp->body_buffer_size();
            std::string len_str;
            if (body_sz > 0) {
                len_str = std::to_string(body_sz);
                resp_headers.emplace_back("content-length", len_str);
            }

            const bool empty_body = (body_sz == 0);
            session->send_headers(stream_id, resp_headers, empty_body);

            if (!empty_body) {
                session->send_data(stream_id, resp->take_body_output_buffer(), true);
            }

            streams.erase(it);
            return true;
        }

        void setup_h2_bridges(const std::function<bool(HttpSessionContext *)> &dispatch_fn,
                              const std::shared_ptr<net::Connection> &conn,
                              const std::shared_ptr<http2::Session> &http2_session,
                              const std::shared_ptr<std::unordered_map<std::uint32_t, Http2AssembledStream>> &h2_streams)
        {
            if (!dispatch_fn || !conn || !http2_session || !h2_streams) {
                return;
            }

            http2_session->set_headers_bridge([h2_streams, http2_session, dispatch_fn, conn](std::uint32_t stream_id, std::string_view raw_headers, bool end_stream) {
                auto &s = (*h2_streams)[stream_id];
                if (s.headers_done) {
                    s.trailers = true;
                    std::unordered_map<std::string, std::string> trailer_pseudo;
                    std::unordered_map<std::string, std::string> trailer_regular;
                    parse_h2_header_block(raw_headers, trailer_pseudo, trailer_regular);
                    for (auto &[k, v] : trailer_regular) {
                        s.regular_headers[std::move(k)] = std::move(v);
                    }
                } else {
                    parse_h2_header_block(raw_headers, s.pseudo_headers, s.regular_headers);
                    s.headers_done = true;
                }
                s.end_stream = s.end_stream || end_stream;
                maybe_log_h2_stream_complete(stream_id, *h2_streams);
                if (s.end_stream) {
                    (void)maybe_reply_h2_via_dispatcher(stream_id, *h2_streams, http2_session, conn, dispatch_fn);
                }
            });

            http2_session->set_data_bridge([h2_streams, http2_session, dispatch_fn, conn](std::uint32_t stream_id, const std::vector<std::uint8_t> &data, bool end_stream) {
                auto it = h2_streams->find(stream_id);
                if (it == h2_streams->end()) {
                    return;
                }
                auto &s = it->second;
                if (!s.body_oversized && s.body.size() + data.size() <= Http2AssembledStream::kMaxBodySize) {
                    s.body.append(reinterpret_cast<const char *>(data.data()), data.size());
                } else {
                    s.body_oversized = true;
                }
                s.end_stream = s.end_stream || end_stream;
                maybe_log_h2_stream_complete(stream_id, *h2_streams);
                (void)maybe_reply_h2_via_dispatcher(stream_id, *h2_streams, http2_session, conn, dispatch_fn);
            });
        }

        bool frame_is_stream_headers(const std::string &hdr)
        {
            return hdr.size() >= 9 && static_cast<unsigned char>(hdr[3]) == 0x01;
        }

        bool frame_is_stream_data(const std::string &hdr)
        {
            return hdr.size() >= 9 && static_cast<unsigned char>(hdr[3]) == 0x00;
        }

        std::uint32_t frame_stream_id(const std::string &hdr)
        {
            if (hdr.size() < 9) {
                return 0;
            }
            const auto b5 = static_cast<std::uint32_t>(static_cast<unsigned char>(hdr[5]));
            const auto b6 = static_cast<std::uint32_t>(static_cast<unsigned char>(hdr[6]));
            const auto b7 = static_cast<std::uint32_t>(static_cast<unsigned char>(hdr[7]));
            const auto b8 = static_cast<std::uint32_t>(static_cast<unsigned char>(hdr[8]));
            return ((b5 << 24) | (b6 << 16) | (b7 << 8) | b8) & 0x7fffffffU;
        }

        bool frame_end_stream_flag(const std::string &hdr)
        {
            return hdr.size() >= 9 && ((static_cast<unsigned char>(hdr[4]) & 0x01) != 0);
        }


        bool load_precompressed_asset(const std::string &source_path,
                                      std::string_view encoding,
                                      std::string &out_body)
        {
            if (source_path.empty() || encoding.empty()) {
                return false;
            }

            const std::string candidate_path = source_path + "." + std::string(encoding);
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::path(std::u8string(candidate_path.begin(), candidate_path.end())), ec) || ec) {
                return false;
            }

            out_body = read_file_to_string(candidate_path);
            if (out_body.empty()) {
                return false;
            }
            return true;
        }
    }

    HttpServer::HttpServer()
        : HttpServer(HttpServerConfig())
    {
    }

    HttpServer::HttpServer(const HttpServerConfig & config)
        : config_(config)
    {
        if (config_.enable_cors) {
            global_pipeline_.add(middlewares::cors());
        }
        if (config_.max_body_size > 0) {
            global_pipeline_.add(middlewares::body_limit(config_.max_body_size));
        }

        config::enable_http2 = config_.enable_http2;
        config::enable_http3 = config_.enable_http3;
        max_connections_limit_.store(config_.max_connections, std::memory_order_relaxed);
        max_connections_per_ip_limit_.store(config_.max_connections_per_ip, std::memory_order_relaxed);
        max_inflight_requests_per_ip_limit_.store(config_.max_inflight_requests_per_ip, std::memory_order_relaxed);

        on("/__proxy_stats", [this](HttpRequest *req, HttpResponse *resp) {
            (void)req;
            auto *proxy = get_proxy();
            if (!proxy) {
                resp->json("{}", ResponseCode::ok_);
                if (req->get_version() != HttpVersion::v_2_0) {
                    resp->send();
                }
                return;
            }

            const auto st = proxy->snapshot_stats();
            const auto sst = snapshot_server_stats();
            nlohmann::json j;
            j["total_requests"] = st.total_requests;
            j["active_connections"] = st.active_connections;
            j["failed_requests"] = st.failed_requests;
            j["pool_hits"] = st.pool_hits;
            j["pool_misses"] = st.pool_misses;
            j["ws_duplicate_upgrade_skipped"] = st.ws_duplicate_upgrade_skipped;
            j["ws_stale_upgrade_skipped"] = st.ws_stale_upgrade_skipped;
            j["unmapped_close_events"] = st.unmapped_close_events;
            j["connection_rejected_total"] = sst.connection_rejected_total;
            j["inflight_rejected_total"] = sst.inflight_rejected_total;
            j["active_http_connections"] = sst.active_http_connections;
            j["active_inflight_requests"] = sst.active_inflight_requests;
            resp->json(j.dump(), ResponseCode::ok_);
            if (req->get_version() != HttpVersion::v_2_0) {
                resp->send();
            }
        });

        on("/__mini_nginx_stats", [this](HttpRequest *req, HttpResponse *resp) {
            nlohmann::json j;

            const auto sst = snapshot_server_stats();
            j["server"]["active_http_connections"] = sst.active_http_connections;
            j["server"]["active_inflight_requests"] = sst.active_inflight_requests;
            j["server"]["connection_rejected_total"] = sst.connection_rejected_total;
            j["server"]["inflight_rejected_total"] = sst.inflight_rejected_total;
            j["server"]["max_connections"] = config_.max_connections;
            j["server"]["max_connections_per_ip"] = config_.max_connections_per_ip;
            j["server"]["max_inflight_requests_per_ip"] = config_.max_inflight_requests_per_ip;
            j["server"]["enable_http2"] = config_.enable_http2;
            j["server"]["enable_http3"] = config_.enable_http3;
            j["server"]["enable_keep_alive"] = config_.enable_keep_alive;

            const auto reject_counters = snapshot_route_reject_counters();
            j["server"]["route_reject_counters"] = nlohmann::json::array();
            for (const auto &item : reject_counters) {
                nlohmann::json r;
                r["route"] = item.route;
                r["rate_limit"] = item.rate_limit;
                r["inflight"] = item.inflight;
                r["conn_reject"] = item.conn_reject;
                j["server"]["route_reject_counters"].push_back(std::move(r));
            }

            auto *proxy = get_proxy();
            if (proxy) {
                const auto pst = proxy->snapshot_stats();
                j["proxy"]["total_requests"] = pst.total_requests;
                j["proxy"]["active_connections"] = pst.active_connections;
                j["proxy"]["failed_requests"] = pst.failed_requests;
                j["proxy"]["pool_hits"] = pst.pool_hits;
                j["proxy"]["pool_misses"] = pst.pool_misses;
                j["proxy"]["ws_duplicate_upgrade_skipped"] = pst.ws_duplicate_upgrade_skipped;
                j["proxy"]["ws_stale_upgrade_skipped"] = pst.ws_stale_upgrade_skipped;
                j["proxy"]["unmapped_close_events"] = pst.unmapped_close_events;

                const auto &routes = proxy->get_routes();
                j["proxy"]["routes_count"] = routes.size();
                j["proxy"]["routes"] = nlohmann::json::array();
                for (const auto &kv : routes) {
                    nlohmann::json r;
                    r["path"] = kv.first;
                    r["targets"] = kv.second.targets.size();
                    r["max_retries"] = kv.second.max_retries;
                    r["failure_threshold"] = kv.second.failure_threshold;
                    r["unhealthy_cooldown_ms"] = kv.second.unhealthy_cooldown_ms;
                    j["proxy"]["routes"].push_back(std::move(r));
                }

                const auto health = proxy->snapshot_target_health();
                j["proxy"]["target_health_count"] = health.size();
                j["proxy"]["target_health"] = nlohmann::json::array();
                for (const auto &item : health) {
                    nlohmann::json h;
                    h["target"] = item.target_key;
                    h["healthy"] = item.healthy;
                    h["consecutive_failures"] = item.consecutive_failures;
                    h["unhealthy_until_ms"] = item.unhealthy_until_ms;
                    j["proxy"]["target_health"].push_back(std::move(h));
                }
            }

            resp->json(j.dump(), ResponseCode::ok_);
            if (req->get_version() != HttpVersion::v_2_0) {
                resp->send();
            }
        });
    }

    HttpServer::~HttpServer()
    {
        stop();
        clear_sessions();
    }

    bool HttpServer::init_ssl_if_needed()
    {
#ifdef HTTP_USE_SSL
        if (!config_.enable_ssl) {
            return true;
        }

        auto resolve_asset = [](const std::filesystem::path &relative) {
            std::error_code ec;
            std::filesystem::path probe = std::filesystem::current_path(ec);
            if (ec || probe.empty()) {
                probe = std::filesystem::path(".");
            }

            for (int depth = 0; depth < 6; ++depth) {
                const auto candidate = probe / relative;
                if (std::filesystem::exists(candidate)) {
                    return candidate;
                }
                if (!probe.has_parent_path()) {
                    break;
                }
                probe = probe.parent_path();
            }

            return relative;
        };

        const auto cert_path = config_.ssl_certificate.empty()
            ? resolve_asset(std::filesystem::path("ca") / "ca.crt")
            : std::filesystem::path(config_.ssl_certificate);
        const auto key_path = config_.ssl_certificate_key.empty()
            ? resolve_asset(std::filesystem::path("ca") / "ca.key")
            : std::filesystem::path(config_.ssl_certificate_key);

        ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!ssl_module_->init(cert_path.string(), key_path.string(), SSLHandler::SSLMode::acceptor_)) {
            if (auto msg = ssl_module_->get_error_message()) {
                LOG_ERROR("{}", msg->c_str());
            }
            return false;
        }

        if (config_.enable_http2) {
            ssl_module_->set_alpn_protocols({"h2", "http/1.1"});
        } else {
            ssl_module_->set_alpn_protocols({"http/1.1"});
        }

        listener_.set_ssl_module(ssl_module_);
#else
        if (config_.enable_ssl) {
            LOG_ERROR("HTTP SSL requested but this build was configured with YUAN_ENABLE_HTTP_SSL=OFF");
            return false;
        }
#endif
        return true;
    }

    bool HttpServer::init_http_features()
    {
        try
        {
            config::load_config();
            config_.enable_http2 = config::enable_http2;
            config_.enable_http3 = config::enable_http3;
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

        on("/__http_caps", [](HttpRequest *req, HttpResponse *resp) {
            (void)req;
            nlohmann::json j;
            j["http1"] = true;
            j["http2"] = config::enable_http2;
            j["http3"] = config::enable_http3;
            j["notes"] = (config::enable_http2 || config::enable_http3)
                ? "HTTP/2 and HTTP/3 capability flags enabled (server stack integration pending)"
                : "HTTP/2 and HTTP/3 are not enabled in this build yet";
            resp->json(j.dump(), ResponseCode::ok_);
            if (req->get_version() != HttpVersion::v_2_0) {
                resp->send();
            }
        });

        on("/__h2_echo", [](HttpRequest *req, HttpResponse *resp) {
            nlohmann::json j;
            j["method"] = std::string(req->get_method_string());
            j["path"] = std::string(req->get_raw_url());
            j["body"] = req->body_buffer_text();
            resp->json(j.dump(), ResponseCode::ok_);
            if (req->get_version() != HttpVersion::v_2_0) {
                resp->send();
            }
        });

        on("/__h2_no_content", [](HttpRequest *req, HttpResponse *resp) {
            (void)req;
            resp->set_response_code(ResponseCode::no_content);
            resp->add_header(http_header_key::content_length, "0");
            if (req->get_version() != HttpVersion::v_2_0) {
                resp->send();
            }
        });
        refresh_h2_dispatch_paths();
    }

    void HttpServer::refresh_h2_dispatch_paths()
    {
        h2_dispatch_paths_.clear();
        h2_dispatch_paths_.insert("/__http_caps");
        h2_dispatch_paths_.insert("/__proxy_stats");
        h2_dispatch_paths_.insert("/__mini_nginx_stats");
        h2_dispatch_paths_.insert("/__h2_echo");
        h2_dispatch_paths_.insert("/__h2_no_content");

        auto cfg = HttpConfigManager::get_instance();
        for (const auto &path : cfg->get_type_array_properties<std::string>("h2_dispatch_paths")) {
            if (!path.empty() && path.front() == '/') {
                h2_dispatch_paths_.insert(path);
            }
        }
    }

    bool HttpServer::init_proxy_if_needed()
    {
        const auto &proxiesCfg = HttpConfigManager::get_instance()->get_type_array_properties<nlohmann::json>("proxies");
        if (proxiesCfg.empty()) {
            return true;
        }

        auto *proxy = ensure_proxy();
        if (!proxy) {
            LOG_ERROR("proxy config exists, but no HTTP proxy factory is installed");
            return false;
        }

        if (!proxy->load_proxy_config_and_init()) {
            LOG_ERROR("load proxies config failed!");
            proxy_.reset();
            return false;
        }

        return true;
    }

    void HttpServer::cleanup_stale_upload_sessions()
    {
        if (upload_session_count_.load(std::memory_order_relaxed) == 0) {
            return;
        }

        const uint64_t observed_last_ms = upload_cleanup_last_ms_.load(std::memory_order_relaxed);
        if (observed_last_ms != 0 &&
            (upload_cleanup_probe_count_.fetch_add(1, std::memory_order_relaxed) & 1023U) != 0U) {
            return;
        }

        const uint64_t observed_now_ms = yuan::base::time::steady_now_ms();
        if (observed_last_ms != 0 && observed_now_ms < observed_last_ms + kUploadCleanupIntervalMs) {
            return;
        }

        std::lock_guard<std::mutex> upload_lock(upload_mutex_);
        const uint64_t now_ms = yuan::base::time::steady_now_ms();
        const uint64_t last_ms = upload_cleanup_last_ms_.load(std::memory_order_relaxed);
        if (last_ms != 0 && now_ms < last_ms + kUploadCleanupIntervalMs) {
            return;
        }
        upload_cleanup_last_ms_.store(now_ms, std::memory_order_relaxed);

        std::vector<std::string> stale_ids;
        stale_ids.reserve(uploaded_chunks_.size());
        for (const auto &entry : uploaded_chunks_) {
            const auto &session = entry.second;
            const uint64_t active_at = session.last_active_ms > 0 ? session.last_active_ms : session.created_at_ms;
            if (active_at == 0 || now_ms <= active_at || now_ms - active_at <= kUploadSessionTtlMs) {
                continue;
            }
            stale_ids.push_back(entry.first);
        }

        std::unordered_set<std::string> stale_id_set;
        stale_id_set.reserve(stale_ids.size());
        std::size_t removed_chunks = 0;
        std::size_t removed_session_tmp_files = 0;

        for (const auto &id : stale_ids) {
            auto it = uploaded_chunks_.find(id);
            if (it == uploaded_chunks_.end()) {
                continue;
            }
            stale_id_set.insert(id);

            for (const auto &chunk_it : it->second.received) {
                const auto &tmp_path = chunk_it.second.tmp_path;
                if (!tmp_path.empty()) {
                    std::error_code ec;
                    std::filesystem::remove(std::filesystem::path(tmp_path), ec);
                    if (!ec) {
                        ++removed_session_tmp_files;
                    }
                }
                ++removed_chunks;
            }

            uploaded_chunks_.erase(it);
            upload_session_count_.store(static_cast<uint32_t>(uploaded_chunks_.size()), std::memory_order_relaxed);
        }

        std::size_t orphan_removed = 0;
        std::error_code dir_ec;
        const std::filesystem::path tmp_dir(".upload_tmp");
        if (std::filesystem::exists(tmp_dir, dir_ec) && !dir_ec) {
            for (const auto &entry : std::filesystem::directory_iterator(tmp_dir, dir_ec)) {
                if (dir_ec || !entry.is_regular_file()) {
                    continue;
                }

                const std::string file_name = entry.path().filename().string();
                const uint64_t age_ms = file_age_ms(entry.path());

                bool should_remove = false;
                std::string parsed_upload_id;
                if (parse_upload_tmp_upload_id(file_name, parsed_upload_id)) {
                    const bool is_active_session = uploaded_chunks_.contains(parsed_upload_id);
                    const bool belongs_to_stale_session = stale_id_set.contains(parsed_upload_id);
                    should_remove = belongs_to_stale_session || (!is_active_session && age_ms >= kUploadTmpFileTtlMs);
                } else {
                    should_remove = age_ms >= kUploadTmpFileTtlMs;
                }

                if (!should_remove) {
                    continue;
                }

                std::error_code rm_ec;
                std::filesystem::remove(entry.path(), rm_ec);
                if (!rm_ec) {
                    ++orphan_removed;
                }
            }
        }

        if (!stale_ids.empty() || orphan_removed > 0 || removed_session_tmp_files > 0) {
            LOG_INFO(
                "[HttpServer] upload cleanup: stale_sessions={}, stale_chunks={}, removed_session_tmp_files={}, removed_orphan_tmp_files={}",
                stale_ids.size(),
                removed_chunks,
                removed_session_tmp_files,
                orphan_removed);
        }

    }

    HttpProxyHandler *HttpServer::ensure_proxy()
    {
        if (proxy_) {
            return &*proxy_;
        }
        if (!proxy_factory_) {
            return nullptr;
        }
        proxy_ = proxy_factory_(*this);
        if (proxy_) {
            proxy_->set_server(this);
        }
        return proxy_ ? &*proxy_ : nullptr;
    }

    void HttpServer::set_proxy_factory(ProxyFactory factory)
    {
        proxy_factory_ = std::move(factory);
    }

    void HttpServer::set_proxy_handler(std::unique_ptr<HttpProxyHandler> proxy)
    {
        proxy_ = std::move(proxy);
        if (proxy_) {
            proxy_->set_server(this);
        }
    }

    void HttpServer::set_listen_options(const ::yuan::net::ListenOptions &options)
    {
        config_.listen_options = options;
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

        if (!validate_request_version(context)) {
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

        if (!validate_request_version(context)) {
            return false;
        }

        return context->is_completed();
    }

    bool HttpServer::parse_request(HttpSessionContext *context, ::yuan::buffer::ByteBuffer &&data)
    {
        if (!context->parse_from(std::move(data))) {
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

        if (!validate_request_version(context)) {
            return false;
        }

        return context->is_completed();
    }

    bool HttpServer::validate_request_version(HttpSessionContext * context)
    {
        if (!context) {
            return false;
        }

        auto *request = context->get_request();
        if (!request) {
            return false;
        }

        switch (request->get_version()) {
        case HttpVersion::v_1_0:
        case HttpVersion::v_1_1:
            return true;
        case HttpVersion::v_2_0:
            if (config::enable_http2) {
                return true;
            }
            context->process_error(ResponseCode::http_version_not_supported);
            return false;
        case HttpVersion::v_3_0:
            if (config::enable_http3) {
                return true;
            }
            context->process_error(ResponseCode::http_version_not_supported);
            return false;
        default:
            context->process_error(ResponseCode::http_version_not_supported);
            return false;
        }
    }

    bool HttpServer::dispatch_request(HttpSessionContext * context)
    {
        cleanup_stale_upload_sessions();

        auto *request = context->get_request();
        auto *response = context->get_response();

        if (request->is_options()) {
            handle_options_preflight(request, response);
            return true;
        }

        apply_http1_connection_response_headers(request, response, config_.enable_keep_alive);

        if (!global_pipeline_.empty() && !global_pipeline_.execute(request, response)) {
            if (response && response->get_response_code() == ResponseCode::too_many_requests) {
                increment_reject_counter(reject_route_key(request), RejectReason::rate_limit);
            }
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
            }
            return true;
        }

        const std::string_view dispatch_path = request->get_path();
        if (const auto *handler = dispatcher_.get_handler_ptr(dispatch_path)) {
            (*handler)(request, response);
        } else {
            context->process_error(ResponseCode::not_found);
        }

        return true;
    }

    void HttpServer::finalize_request(uint64_t sessionId, HttpSession * session, HttpSessionContext * context)
    {
        if (context && access_log_hook_) {
            auto *request = context->get_request();
            auto *response = context->get_response();
            if (request && response) {
                access_log_hook_(request, response, sessionId);
            }
        }

        if (config::close_idle_connection && session) {
            session->reset_timer();
        }
    }

    void HttpServer::store_session(uint64_t session_id, std::unique_ptr<HttpSession> session)
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = std::move(session);
    }

    bool HttpServer::has_session(uint64_t session_id) const
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        return sessions_.find(session_id) != sessions_.end();
    }

    void HttpServer::erase_session(uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(session_id);
    }

    void HttpServer::abort_sessions()
    {
        std::vector<std::shared_ptr<net::Connection>> connections;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            connections.reserve(sessions_.size());
            for (auto &[id, session] : sessions_) {
                if (!session) {
                    continue;
                }
                auto *ctx = session->get_context();
                if (!ctx) {
                    continue;
                }
                if (auto conn = ctx->connection()) {
                    connections.push_back(std::move(conn));
                }
            }
        }

        for (auto &conn : connections) {
            if (conn && conn->is_connected()) {
                conn->abort();
            }
        }
    }

    void HttpServer::clear_sessions()
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }

    bool HttpServer::init(int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return init(port, *owned_runtime_);
    }

    bool HttpServer::init(int port, NetworkRuntime & runtime)
    {
        clear_sessions();
        uploaded_chunks_.clear();
        upload_session_count_.store(0, std::memory_order_relaxed);
        proxy_.reset();
        listener_.close();

        config::enable_http2 = config_.enable_http2;
        config::enable_http3 = config_.enable_http3;

        if (HttpConfigManager::get_instance()->good()) {
            LOG_INFO("{} starting...", HttpConfigManager::get_instance()->get_string_property("server_name"));
        }

        if (!init_ssl_if_needed()) {
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        if (!listener_.bind(static_cast<uint16_t>(port), runtime, config_.listen_options)) {
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

        auto accept_task = listener_.run_async();
        accept_task.resume();
        accept_task.detach();

        if (owned_runtime_) {
            owned_runtime_->run();
        }
    }

    void HttpServer::stop()
    {
        abort_sessions();

        listener_.close();
        if (owned_runtime_) {
            owned_runtime_->stop();
        }

        if (thread_pool_) {
            thread_pool_->shutdown();
        }

        clear_sessions();
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

    bool HttpServer::is_h2_dispatch_path(std::string_view path) const
    {
        return h2_dispatch_paths_.contains(std::string(path));
    }

    bool HttpServer::allow_new_connection(const net::Connection &conn)
    {
        const int max_connections = max_connections_limit_.load(std::memory_order_relaxed);
        const int max_connections_per_ip = max_connections_per_ip_limit_.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(conn_limit_mutex_);
        if (max_connections > 0) {
            int total = 0;
            for (const auto &kv : active_conn_per_ip_) {
                total += kv.second;
            }
            if (total >= max_connections) {
                return false;
            }
        }

        const uint32_t ip = conn.get_remote_address().get_net_ip();
        if (max_connections_per_ip > 0) {
            const int cur = active_conn_per_ip_[ip];
            if (cur >= max_connections_per_ip) {
                return false;
            }
        }

        ++active_conn_per_ip_[ip];
        return true;
    }

    void HttpServer::on_connection_closed(const net::Connection &conn)
    {
        const uint32_t ip = conn.get_remote_address().get_net_ip();

        std::lock_guard<std::mutex> lock(conn_limit_mutex_);
        auto it = active_conn_per_ip_.find(ip);
        if (it != active_conn_per_ip_.end()) {
            --it->second;
            if (it->second <= 0) {
                active_conn_per_ip_.erase(it);
            }
        }
    }

    bool HttpServer::try_acquire_inflight_request(const HttpRequest *request, bool *tracked)
    {
        if (tracked) {
            *tracked = false;
        }
        const int max_inflight_requests_per_ip = max_inflight_requests_per_ip_limit_.load(std::memory_order_relaxed);
        if (!request || max_inflight_requests_per_ip <= 0) {
            return true;
        }

        const uint32_t ip = request->get_peer_ip_uint32();
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        int &cur = inflight_req_per_ip_[ip];
        if (cur >= max_inflight_requests_per_ip) {
            return false;
        }
        ++cur;
        if (tracked) {
            *tracked = true;
        }
        return true;
    }

    void HttpServer::release_inflight_request(const HttpRequest *request, bool tracked)
    {
        if (!tracked || !request) {
            return;
        }

        const uint32_t ip = request->get_peer_ip_uint32();
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        auto it = inflight_req_per_ip_.find(ip);
        if (it == inflight_req_per_ip_.end()) {
            return;
        }
        --it->second;
        if (it->second <= 0) {
            inflight_req_per_ip_.erase(it);
        }
    }

    HttpServerStats HttpServer::snapshot_server_stats() const
    {
        HttpServerStats st;
        st.connection_rejected_total = connection_rejected_total_.load(std::memory_order_relaxed);
        st.inflight_rejected_total = inflight_rejected_total_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(conn_limit_mutex_);
            for (const auto &kv : active_conn_per_ip_) {
                st.active_http_connections += kv.second;
            }
        }
        {
            std::lock_guard<std::mutex> lock(inflight_mutex_);
            for (const auto &kv : inflight_req_per_ip_) {
                st.active_inflight_requests += kv.second;
            }
        }
        return st;
    }

    std::vector<RouteRejectCounters> HttpServer::snapshot_route_reject_counters() const
    {
        std::lock_guard<std::mutex> lock(reject_counters_mutex_);
        std::vector<RouteRejectCounters> out;
        out.reserve(reject_counters_.size());
        for (const auto &kv : reject_counters_) {
            out.push_back(kv.second);
        }
        return out;
    }

    void HttpServer::update_runtime_limits(int max_connections,
                                           int max_connections_per_ip,
                                           int max_inflight_requests_per_ip)
    {
        max_connections_limit_.store(max_connections, std::memory_order_relaxed);
        max_connections_per_ip_limit_.store(max_connections_per_ip, std::memory_order_relaxed);
        max_inflight_requests_per_ip_limit_.store(max_inflight_requests_per_ip, std::memory_order_relaxed);
    }

    std::string HttpServer::reject_route_key(const HttpRequest *request) const
    {
        if (!request) {
            return "unknown";
        }

        const std::string raw = request->get_raw_url();
        if (proxy_ && proxy_->is_proxy_url(raw)) {
            const std::string route = proxy_->find_proxy_route(raw);
            if (!route.empty()) {
                return route;
            }
        }

        return std::string(request->get_path());
    }

    void HttpServer::increment_reject_counter(std::string route_key, RejectReason reason)
    {
        if (route_key.empty()) {
            route_key = "unknown";
        }
        std::lock_guard<std::mutex> lock(reject_counters_mutex_);
        auto &c = reject_counters_[route_key];
        c.route = route_key;
        switch (reason) {
        case RejectReason::rate_limit:
            ++c.rate_limit;
            break;
        case RejectReason::inflight:
            ++c.inflight;
            break;
        case RejectReason::conn_reject:
            ++c.conn_reject;
            break;
        }
    }

    bool HttpServer::dispatch_h2_context(HttpSessionContext *context)
    {
        if (!context) {
            return false;
        }

        auto *request = context->get_request();
        auto *response = context->get_response();
        if (!request || !response) {
            return false;
        }

        if (request->is_options()) {
            handle_options_preflight(request, response);
            return true;
        }

        if (!global_pipeline_.empty() && !global_pipeline_.execute(request, response)) {
            if (response && response->get_response_code() == ResponseCode::too_many_requests) {
                increment_reject_counter(reject_route_key(request), RejectReason::rate_limit);
            }
            return true;
        }

        if (proxy_ && proxy_->is_proxy_url(request->get_raw_url())) {
            proxy_->serve_proxy(request, response);
            return true;
        }

        const std::string_view dispatch_path = request->get_path();
        if (const auto *handler = dispatcher_.get_handler_ptr(dispatch_path)) {
            (*handler)(request, response);
        } else {
            response->set_response_code(ResponseCode::not_found);
            response->add_header(http_header_key::content_type, "text/plain");
            response->append_body("not found");
        }
        return true;
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
        auto conn = ctx.connection();
        if (!conn) {
            co_return;
        }

        if (!allow_new_connection(*conn)) {
            ++connection_rejected_total_;
            increment_reject_counter("connection_limit", RejectReason::conn_reject);
            conn->append_output("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            conn->flush();
            conn->close();
            co_return;
        }
        bool connection_counted = true;
        auto release_connection_count = [&]() {
            if (connection_counted && conn) {
                on_connection_closed(*conn);
                connection_counted = false;
            }
        };

        bool alpn_h2 = false;
        if (conn->is_ssl_handshaking()) {
            auto hs_result = co_await ctx.ssl_handshake_async(keep_alive_idle_timeout_ms());
            if (hs_result != coroutine::SslHandshakeResult::success) {
                if (ctx.is_connected()) {
                    ctx.close();
                }
                release_connection_count();
                co_return;
            }

            if (auto ssl = conn->get_ssl_handler()) {
                auto alpn = ssl->get_alpn_selected();
                if (alpn == "h2") {
                    alpn_h2 = true;
                }
            }
        }

        const auto sessionId = ctx.connection_id();
        conn->set_max_packet_size(HttpPacket::get_max_packet_size());

        auto httpCtx = std::make_unique<HttpSessionContext>(conn);
        httpCtx->set_request_timing_enabled(config_.track_request_time || static_cast<bool>(access_log_hook_));
        const bool use_session_idle_timer = config::close_idle_connection && config::connection_idle_timeout > 0;
        auto session = std::make_unique<HttpSession>(
            sessionId,
            std::move(httpCtx),
            ctx.runtime_view(),
            use_session_idle_timer);
        auto *session_ptr = ptr_of(session);
        store_session(sessionId, std::move(session));

        bool protocol_checked = alpn_h2 || !config::enable_http2;
        bool http2_mode = false;
        bool h2_handshake_active = false;
        ::yuan::buffer::ByteBuffer protocol_probe_data;
        ::yuan::buffer::ByteBuffer h2_preface_data;
        std::shared_ptr<http2::Session> http2_session;
        std::chrono::steady_clock::time_point settings_sent_at;
        bool http1_long_lived_response = false;

        auto enter_h2_mode = [&](bool expect_client_preface) {
            auto h2_streams = std::make_shared<std::unordered_map<std::uint32_t, Http2AssembledStream>>();
            http2_session = std::make_shared<http2::Session>(conn);
            auto h2_dispatch = [this](HttpSessionContext *c) {
                return this->dispatch_h2_context(c);
            };
            setup_h2_bridges(h2_dispatch, conn, http2_session, h2_streams);
            if (!http2_session->on_preface_received()) {
                conn->close();
                return false;
            }
            settings_sent_at = std::chrono::steady_clock::now();
            http2_mode = true;
            h2_handshake_active = expect_client_preface;
            return true;
        };

        if (alpn_h2) {
            if (!config::enable_http2) {
                LOG_WARN("ALPN selected h2 but enable_http2=false from {}", conn->get_remote_address().to_address_key());
                if (ctx.is_connected()) {
                    ctx.close();
                }
                release_connection_count();
                co_return;
            }
            LOG_INFO("HTTP/2 via ALPN from {}", conn->get_remote_address().to_address_key());
            if (!enter_h2_mode(true)) {
                release_connection_count();
                co_return;
            }
        }

        while (ctx.is_connected()) {
            const uint32_t read_timeout_ms = (!http2_mode && !http1_long_lived_response)
                ? (use_session_idle_timer ? uint32_t{0} : keep_alive_idle_timeout_ms())
                : uint32_t{0};
            auto read_result = co_await ctx.read_async(read_timeout_ms);
            if (read_result.status == coroutine::IoStatus::timed_out) {
                LOG_DEBUG("HTTP/1 idle read timeout from {}", conn->get_remote_address().to_address_key());
                break;
            }
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }
            session_ptr->reset_timer();

            if (http2_mode) {
                if (h2_handshake_active) {
                    h2_preface_data.append(read_result.data.readable_span());
                    const auto probe = probe_http2_preface(h2_preface_data);
                    if (probe == Http2PrefaceProbe::need_more) {
                        continue;
                    }
                    if (probe == Http2PrefaceProbe::not_preface) {
                        LOG_WARN("Invalid HTTP/2 client preface from {}", conn->get_remote_address().to_address_key());
                        break;
                    }

                    h2_preface_data.consume(kHttp2ConnectionPreface.size());
                    h2_handshake_active = false;
                    if (h2_preface_data.readable_bytes() == 0) {
                        h2_preface_data.clear();
                        continue;
                    }
                    if (!http2_session || !http2_session->on_bytes(h2_preface_data)) {
                        break;
                    }
                    h2_preface_data.clear();
                    continue;
                }
                if (!http2_session || !http2_session->on_bytes(read_result.data)) {
                    break;
                }
                if (http2_session->awaiting_settings_ack()) {
                    constexpr auto kSettingsAckTimeout = std::chrono::seconds(30);
                    if (std::chrono::steady_clock::now() - settings_sent_at > kSettingsAckTimeout) {
                        LOG_WARN("HTTP/2 SETTINGS ACK timeout from {}", conn->get_remote_address().to_address_key());
                        break;
                    }
                }
                continue;
            }

            ::yuan::buffer::ByteBuffer parse_data;
            if (!protocol_checked) {
                protocol_probe_data.append(read_result.data.readable_span());
                const auto probe = probe_http2_preface(protocol_probe_data);
                if (probe == Http2PrefaceProbe::need_more) {
                    continue;
                }
                if (probe == Http2PrefaceProbe::is_preface) {
                    if (config::enable_http2) {
                        LOG_INFO("HTTP/2 preface accepted from {}", conn->get_remote_address().to_address_key());
                        if (!enter_h2_mode(false)) {
                            break;
                        }

                        protocol_probe_data.consume(kHttp2ConnectionPreface.size());

                        if (!http2_session->on_bytes(protocol_probe_data)) {
                            break;
                        }

                        protocol_probe_data.clear();
                        continue;
                    } else {
                        LOG_WARN("HTTP/2 preface rejected from {} (enable_http2=false)", conn->get_remote_address().to_address_key());
                        session_ptr->get_context()->process_error(ResponseCode::http_version_not_supported);
                    }
                    break;
                }

                protocol_checked = true;
                parse_data = std::move(protocol_probe_data);
            } else {
                parse_data = std::move(read_result.data);
            }

            auto *context = session_ptr->get_context();

            bool inflight_tracked = false;
            try
            {
                if (!parse_request(context, std::move(parse_data))) {
                    if (context->has_error()) {
                        break;
                    }
                    continue;
                }

                const bool inflight_acquired = try_acquire_inflight_request(context->get_request(), &inflight_tracked);
                if (!inflight_acquired) {
                    ++inflight_rejected_total_;
                    increment_reject_counter(reject_route_key(context->get_request()), RejectReason::inflight);
                    context->process_error(ResponseCode::too_many_requests);
                    break;
                }

                (void)dispatch_request(context);
                if (proxy_ && proxy_->is_proxy_url(context->get_request()->get_raw_url())) {
                    co_await proxy_->serve_proxy_async(context->get_request(), context->get_response());
                    if (proxy_->has_client_mapping(conn.get())) {
                        release_inflight_request(context->get_request(), inflight_tracked);
                        release_connection_count();
                        erase_session(sessionId);
                        co_return;
                    }
                }

                if (context->ws_handoff_ && ws_proxy_handler_) {
                    std::string route_key = std::move(context->ws_route_key_);
                    std::string raw_url = context->get_request()->get_raw_url();
                    std::string client_key = std::move(context->ws_client_key_);
                    std::string subproto = std::move(context->ws_subproto_);
                    auto leftover = context->take_leftover_buffer();
                    release_inflight_request(context->get_request(), inflight_tracked);
                    release_connection_count();
                    erase_session(sessionId);

                    auto proxy_task = ws_proxy_handler_(std::move(ctx), raw_url, route_key, client_key, subproto, std::move(leftover));
                    proxy_task.resume();
                    proxy_task.detach();
                    co_return;
                }

                finalize_request(sessionId, session_ptr, context);
                release_inflight_request(context->get_request(), inflight_tracked);

                const bool close_after_response = should_close_http1_connection(
                    context->get_request(), context->get_response(), config_.enable_keep_alive) ||
                    conn->input_shutdown();
                http1_long_lived_response = context->get_response()->is_sse();

                bool response_stream_failed = false;
                const uint32_t write_timeout_ms = response_write_timeout_ms(config_);
                while (context->get_response()->is_uploading()) {
                    auto flush_result = co_await ctx.flush_async(write_timeout_ms);
                    if (flush_result.status != coroutine::IoStatus::success) {
                        response_stream_failed = true;
                        break;
                    }
                    context->write();
                    if (!ctx.is_connected()) {
                        response_stream_failed = true;
                        break;
                    }
                }

                if (response_stream_failed) {
                    if (ctx.is_connected()) {
                        ctx.abort();
                    }
                    break;
                }

                if (close_after_response && ctx.is_connected()) {
                    ctx.close();
                    break;
                }
            }
            catch (const fmt::format_error &e)
            {
                LOG_ERROR("Invalid UTF-8 or format error while processing HTTP request: {}", e.what());
                if (has_session(sessionId)) {
                    release_inflight_request(context->get_request(), inflight_tracked);
                    context->process_error(ResponseCode::bad_request);
                }
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Exception while processing HTTP request: {}", e.what());
                if (has_session(sessionId)) {
                    release_inflight_request(context->get_request(), inflight_tracked);
                    context->process_error(ResponseCode::internal_server_error);
                }
            }
            catch (...)
            {
                LOG_ERROR("Unknown exception while processing HTTP request");
                if (has_session(sessionId)) {
                    release_inflight_request(context->get_request(), inflight_tracked);
                    context->process_error(ResponseCode::internal_server_error);
                }
            }
        }

        if (http2_session) {
            http2_session->close_gracefully();
        }

        if (ctx.is_connected()) {
            ctx.close();
        }

        if (proxy_ && conn) {
            proxy_->on_client_close(conn);
        }
        release_connection_count();
        erase_session(sessionId);

        co_return;
    }

    void HttpServer::load_static_paths()
    {
        static_mount_trie_.clear();
        static_mounts_.clear();

        auto cfgManager = HttpConfigManager::get_instance();
        const std::vector<nlohmann::json> &paths = cfgManager->get_type_array_properties<nlohmann::json>(config::static_file_paths);

        for (const auto &path : paths) {
            const std::string &root = path[config::static_file_paths_root];
            const std::string &rootPath = path[config::static_file_paths_path];
            if (root.empty() || rootPath.empty())
                continue;

            mount_static(root, rootPath);
        }

        if (paths.empty()) {
            mount_static("/static", std::filesystem::current_path().string());
        }

        const std::vector<std::string> &types = cfgManager->get_type_array_properties<std::string>(config::playable_types);
        for (const auto &type : types)
            play_types_.insert(type);

        if (play_types_.empty()) {
            play_types_.insert({ ".mp4", ".mp3", ".mov", ".flac", ".wav", ".avi", ".ogg" });
        }
    }

    void HttpServer::mount_static(const std::string &url_prefix, const std::string &root, StaticMountOptions options)
    {
        if (url_prefix.empty() || root.empty()) {
            return;
        }

        std::string normalized_prefix = url_prefix;
        if (normalized_prefix.front() != '/') {
            normalized_prefix.insert(normalized_prefix.begin(), '/');
        }
        if (normalized_prefix.size() > 1 && normalized_prefix.back() == '/') {
            normalized_prefix.pop_back();
        }

        std::string normalized_root = root;
        try {
            normalized_root = std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(root.begin(), root.end()))).string();
        } catch (...) {
        }

        StaticMount mount;
        mount.prefix = normalized_prefix;
        mount.root = normalized_root;
        mount.options = std::move(options);

        static_mounts_[normalized_prefix] = mount;
        static_mount_trie_.insert(normalized_prefix, true);

        on(normalized_prefix, [this](HttpRequest *req, HttpResponse *resp) {
            this->serve_static(req, resp);
        }, true);
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
        const std::string & request_path,
        const StaticMount *& mount,
        std::string & file_relative_path,
        HttpResponse * resp)
    {
        const auto result = static_mount_trie_.find_prefix(request_path);
        const auto prefix_idx = static_cast<size_t>(result.match_length);

        if (!result || prefix_idx == 0 || !result.is_registered) {
            resp->process_error(ResponseCode::not_found);
            return false;
        }

        const std::string prefix = request_path.substr(0, prefix_idx);
        const auto static_path_it = static_mounts_.find(prefix);
        if (static_path_it == static_mounts_.end() || static_path_it->second.root.empty()) {
            resp->process_error(ResponseCode::not_found);
            return false;
        }
        mount = &static_path_it->second;

        if (request_path.size() <= prefix_idx) {
            file_relative_path.clear();
            return true;
        }

        if (request_path[prefix_idx] != '/') {
            resp->process_error(ResponseCode::not_found);
            return false;
        }

        if (request_path.size() <= prefix_idx + 1) {
            file_relative_path.clear();
            return true;
        }

        file_relative_path = request_path.substr(prefix_idx + 1);
        if (file_relative_path.find("..") != std::string::npos ||
            file_relative_path.find('\\') != std::string::npos ||
            (!file_relative_path.empty() && file_relative_path.front() == '/')) {
            resp->process_error(ResponseCode::forbidden);
            return false;
        }

        return true;
    }

    std::string HttpServer::make_weak_etag(const std::filesystem::path &path, std::size_t size, std::time_t modified_at)
    {
        const auto name_hash = std::hash<std::string>{}(path.filename().string());
        return "W/\"" + std::to_string(size) + "-" + std::to_string(static_cast<long long>(modified_at)) +
               "-" + std::to_string(static_cast<unsigned long long>(name_hash)) + "\"";
    }

    std::string HttpServer::format_http_date(std::time_t ts)
    {
        std::tm gmt{};
#ifdef _WIN32
        gmtime_s(&gmt, &ts);
#else
        gmtime_r(&ts, &gmt);
#endif
        std::ostringstream oss;
        oss << std::put_time(&gmt, "%a, %d %b %Y %H:%M:%S GMT");
        return oss.str();
    }

    bool HttpServer::parse_http_date(std::string_view text, std::time_t &out)
    {
        std::tm tm{};
        std::istringstream iss{ std::string(text) };
        iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
        if (iss.fail()) {
            return false;
        }
#ifdef _WIN32
        out = _mkgmtime(&tm);
#else
        out = timegm(&tm);
#endif
        return out != static_cast<std::time_t>(-1);
    }

    bool HttpServer::should_return_not_modified(HttpRequest *req, const std::string &etag, std::time_t modified_at)
    {
        if (!req) {
            return false;
        }

        if (const auto *if_none_match = req->get_header(http_header_key::if_none_match)) {
            std::size_t pos = 0;
            while (pos < if_none_match->size()) {
                std::size_t comma = if_none_match->find(',', pos);
                if (comma == std::string::npos) {
                    comma = if_none_match->size();
                }
                const std::string_view token = trim_ascii(std::string_view(*if_none_match).substr(pos, comma - pos));
                if (!token.empty() && (token == etag || token == "*")) {
                    return true;
                }
                pos = comma + 1;
            }
        }

        if (const auto *if_modified_since = req->get_header(http_header_key::if_modified_since)) {
            std::time_t since = 0;
            const std::string_view date_text = trim_ascii(*if_modified_since);
            if (!date_text.empty() && parse_http_date(date_text, since) && modified_at <= since) {
                return true;
            }
        }

        return false;
    }

    bool HttpServer::maybe_compress_static_response(HttpRequest *req,
                                                    HttpResponse *resp,
                                                    const std::string &content_type,
                                                    const std::string &source_path,
                                                    std::size_t source_length)
    {
        if (!req || !resp) {
            return false;
        }

        const auto *accept_encoding = req->get_header(http_header_key::accept_encoding);
        if (!accept_encoding || accept_encoding->empty()) {
            return false;
        }

        if (source_length > kStaticCompressionBufferLimit) {
            return false;
        }

        if (!is_textual_content_type(content_type)) {
            return false;
        }

        const auto preferred = parse_accept_encoding_preferred(*accept_encoding);
        if (!preferred.has_value()) {
            return false;
        }

        std::string precompressed_body;
        if (iequals_ascii(*preferred, "br") && load_precompressed_asset(source_path, "br", precompressed_body)) {
            resp->append_body(std::move(precompressed_body));
            resp->add_header(http_header_key::content_encoding, "br");
            resp->add_header(http_header_key::vary, "accept-encoding");
            return true;
        }

        if (iequals_ascii(*preferred, "gzip") && load_precompressed_asset(source_path, "gz", precompressed_body)) {
            resp->append_body(std::move(precompressed_body));
            resp->add_header(http_header_key::content_encoding, "gzip");
            resp->add_header(http_header_key::vary, "accept-encoding");
            return true;
        }

        if (source_length < 256) {
            return false;
        }

        const std::string plain = read_file_to_string(source_path);
        if (plain.empty()) {
            return false;
        }

#if YUAN_HTTP_HAS_BROTLI
        if (iequals_ascii(*preferred, "br")) {
            const size_t max_size = BrotliEncoderMaxCompressedSize(plain.size());
            if (max_size == 0) {
                return false;
            }

            std::string compressed;
            compressed.resize(max_size);
            size_t encoded_size = max_size;
            const BROTLI_BOOL ok = BrotliEncoderCompress(
                5,
                BROTLI_DEFAULT_WINDOW,
                BROTLI_MODE_TEXT,
                plain.size(),
                reinterpret_cast<const uint8_t *>(plain.data()),
                &encoded_size,
                reinterpret_cast<uint8_t *>(&compressed[0]));
            if (ok == BROTLI_TRUE && encoded_size > 0 && encoded_size < plain.size()) {
                compressed.resize(encoded_size);
                resp->append_body(std::move(compressed));
                resp->add_header(http_header_key::content_encoding, "br");
                resp->add_header(http_header_key::vary, "accept-encoding");
                return true;
            }
        }
#endif

#if !YUAN_HTTP_HAS_ZLIB
        return false;
#else
        if (!iequals_ascii(*preferred, "gzip")) {
            return false;
        }

        z_stream zs{};
        if (deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return false;
        }

        std::string compressed;
        compressed.resize(compressBound(static_cast<uLong>(plain.size())));

        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(plain.data()));
        zs.avail_in = static_cast<uInt>(plain.size());
        zs.next_out = reinterpret_cast<Bytef *>(&compressed[0]);
        zs.avail_out = static_cast<uInt>(compressed.size());

        const int rc = deflate(&zs, Z_FINISH);
        deflateEnd(&zs);
        if (rc != Z_STREAM_END) {
            return false;
        }

        compressed.resize(zs.total_out);
        if (compressed.size() >= plain.size()) {
            return false;
        }

        resp->append_body(std::move(compressed));
        resp->add_header(http_header_key::content_encoding, "gzip");
        resp->add_header(http_header_key::vary, "accept-encoding");
        return true;
#endif
    }

    bool HttpServer::serve_embedded_static_page(const std::string & file_relative_path, HttpResponse * resp, const StaticMountOptions &options)
    {
        if (file_relative_path == "upload") {
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
        const StaticMount & mount,
        const std::string & file_relative_path,
        const std::string & full_path)
    {
        const std::string &path = full_path;

        try
        {
            if (std::filesystem::is_directory(std::filesystem::path(std::u8string(path.begin(), path.end())))) {
                if (mount.options.auto_index) {
                    const std::string request_path = req ? std::string(req->get_path()) : mount.prefix;
                    const bool as_json = req && req->get_request_params().contains("json");
                    serve_list_files(mount.root, path, request_path, resp, as_json);
                } else {
                    resp->process_error(ResponseCode::forbidden);
                }
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
        if (req && req->get_request_params().contains("justDownload")) {
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
        if (file_size < 0) {
            resp->process_error(ResponseCode::bad_request);
            return;
        }

        const auto length = static_cast<std::size_t>(file_size);
        const std::string content_type = get_content_type(ext);
        if (content_type.empty()) {
            resp->process_error(ResponseCode::bad_request);
            return;
        }

        uint64_t offset = 0;
        uint64_t range_end = 0;
        bool has_explicit_range_end = false;
        bool has_range = false;
        if (mount.options.enable_range) {
            if (const std::string *range = req->get_header(http_header_key::range)) {
            int ret = 0;
            const auto &ranges = helper::parse_range(*range, ret);
            if (ret == 0 && !ranges.empty()) {
                offset = ranges[0].first;
                if (ranges[0].second >= offset) {
                    range_end = ranges[0].second;
                    has_explicit_range_end = true;
                }
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
        }

        const uint64_t effective_end = has_explicit_range_end
            ? (std::min)(range_end, static_cast<uint64_t>(length - 1))
            : static_cast<uint64_t>(length == 0 ? 0 : length - 1);
        const auto sz = length == 0 ? std::size_t{0} : static_cast<std::size_t>(effective_end - offset + 1);

        std::error_code stat_ec;
        const auto write_time = std::filesystem::last_write_time(std::filesystem::path(std::u8string(path.begin(), path.end())), stat_ec);
        std::time_t modified_at = std::time(nullptr);
        if (!stat_ec) {
            const auto sys_tp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                write_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            modified_at = std::chrono::system_clock::to_time_t(sys_tp);
        }
        const std::string etag = make_weak_etag(std::filesystem::path(std::u8string(path.begin(), path.end())), length, modified_at);

        if (!has_range && should_return_not_modified(req, etag, modified_at)) {
            resp->set_response_code(ResponseCode::not_modified);
            resp->add_header(http_header_key::etag, etag);
            resp->add_header(http_header_key::last_modified, format_http_date(modified_at));
            resp->add_header("Cache-Control", "no-cache");
            resp->add_header("Content-Length", "0");
            resp->send();
            return;
        }

        if (has_range) {
            resp->add_header("Content-Range",
                             "bytes " + std::to_string(offset) + "-" +
                                  std::to_string(effective_end) + "/" + std::to_string(length));
            resp->set_response_code(ResponseCode::partial_content);
        } else {
            resp->set_response_code(ResponseCode::ok_);
        }

        resp->add_header("Content-Type", content_type);
        resp->add_header("Content-Disposition", "inline; filename=\"" + url::url_encode(file_relative_path) + "\"");
        resp->add_header(http_header_key::etag, etag);
        resp->add_header(http_header_key::last_modified, format_http_date(modified_at));
        resp->add_header("Accept-Ranges", "bytes");
        resp->add_header("X-Content-Type-Options", "nosniff");
        resp->add_header("Cache-Control", "no-cache");

        stream.close();

        if (req && req->get_method() == HttpMethod::head_) {
            resp->add_header("Content-Length", std::to_string(sz));
            resp->send();
            return;
        }

        if (!has_range && maybe_compress_static_response(req, resp, content_type, path, sz)) {
            resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
            resp->send();
            return;
        }

        resp->add_header("Content-Length", std::to_string(sz));
        if (sz == 0) {
            resp->send();
            return;
        }

        auto task = std::make_unique<net::http::HttpUploadFileTask>([resp]() {
            resp->set_upload_file(false);
        });

        const auto attachment_info = std::make_shared<net::http::AttachmentInfo>();
        attachment_info->origin_file_name_ = path;
        attachment_info->source_offset_ = static_cast<std::size_t>(offset);
        attachment_info->offset_ = 0;
        attachment_info->length_ = sz;
        task->set_attachment_info(attachment_info);

        if (!task->init()) {
            resp->process_error(ResponseCode::internal_server_error);
            return;
        }

        resp->set_task(task.release());
        resp->set_upload_file(true);
        resp->send();
    }

    void HttpServer::serve_static(HttpRequest * req, HttpResponse * resp)
    {
        const std::string request_path = std::string(req->get_path());
        const StaticMount *mount = nullptr;
        std::string file_relative_path;
        if (!resolve_static_request(request_path, mount, file_relative_path, resp) || !mount) {
            return;
        }

        std::string full_path = mount->root;
        if (!file_relative_path.empty()) {
            full_path += "/";
            full_path += file_relative_path;
        }

        if (!mount->options.try_files.empty()) {
            int forced_status = 0;
            for (const auto &candidate : mount->options.try_files) {
                if (!candidate.empty() && candidate.front() == '=') {
                    try {
                        forced_status = std::stoi(candidate.substr(1));
                    } catch (...) {
                        forced_status = 0;
                    }
                    break;
                }

                std::string replaced = candidate;
                const std::string marker = "$uri";
                const std::string uri_value = "/" + file_relative_path;
                std::size_t pos = 0;
                while ((pos = replaced.find(marker, pos)) != std::string::npos) {
                    replaced.replace(pos, marker.size(), uri_value);
                    pos += uri_value.size();
                }

                std::string rel = replaced;
                if (!rel.empty() && rel.front() == '/') {
                    rel.erase(rel.begin());
                }

                std::string candidate_full = mount->root;
                if (!rel.empty()) {
                    if (!candidate_full.empty() && candidate_full.back() != '/' && candidate_full.back() != '\\') {
                        candidate_full += '/';
                    }
                    candidate_full += rel;
                }

                try {
                    if (std::filesystem::exists(std::filesystem::path(std::u8string(candidate_full.begin(), candidate_full.end())))) {
                        file_relative_path = rel;
                        full_path = candidate_full;
                        break;
                    }
                } catch (...) {
                }
            }

            if (forced_status > 0 &&
                !std::filesystem::exists(std::filesystem::path(std::u8string(full_path.begin(), full_path.end())))) {
                auto ep_it = mount->options.error_pages.find(forced_status);
                if (ep_it != mount->options.error_pages.end()) {
                    std::string ep = ep_it->second;
                    if (!ep.empty() && ep.front() == '/') {
                        ep.erase(ep.begin());
                    }
                    std::string ep_path = mount->root;
                    if (!ep.empty()) {
                        if (!ep_path.empty() && ep_path.back() != '/' && ep_path.back() != '\\') {
                            ep_path += '/';
                        }
                        ep_path += ep;
                    }
                    if (std::filesystem::exists(std::filesystem::path(std::u8string(ep_path.begin(), ep_path.end())))) {
                        serve_static_file(req, resp, *mount, ep, ep_path);
                        return;
                    }
                }

                resp->process_error(static_cast<ResponseCode>(forced_status));
                return;
            }
        }

        try {
            std::filesystem::path canonical_root = std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(mount->root.begin(), mount->root.end())));
            std::filesystem::path canonical_target = std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(full_path.begin(), full_path.end())));
            const auto root_str = canonical_root.string();
            const auto target_str = canonical_target.string();
            if (target_str.compare(0, root_str.size(), root_str) != 0) {
                resp->process_error(ResponseCode::forbidden);
                return;
            }
            full_path = canonical_target.string();
        } catch (...) {
            resp->process_error(ResponseCode::forbidden);
            return;
        }

        if (std::filesystem::is_directory(std::filesystem::path(std::u8string(full_path.begin(), full_path.end())))) {
            for (const auto &index_file : mount->options.index_files) {
                std::string index_path = full_path;
                if (!index_path.empty() && index_path.back() != '/' && index_path.back() != '\\') {
                    index_path += '/';
                }
                index_path += index_file;
                if (std::filesystem::exists(std::filesystem::path(std::u8string(index_path.begin(), index_path.end())))) {
                    const std::string rel = file_relative_path.empty() ? index_file : (file_relative_path + "/" + index_file);
                    serve_static_file(req, resp, *mount, rel, index_path);
                    return;
                }
            }
        }

        if (file_relative_path.empty()) {
            if (mount->options.auto_index) {
                const bool as_json = req->get_request_params().contains("json");
                serve_list_files(mount->root, mount->root, request_path, resp, as_json);
            } else {
                resp->process_error(ResponseCode::forbidden);
            }
            return;
        }

        if (serve_embedded_static_page(file_relative_path, resp, mount->options)) {
            return;
        }

        if (!std::filesystem::exists(std::filesystem::path(std::u8string(full_path.begin(), full_path.end())))) {
            auto ep_it = mount->options.error_pages.find(404);
            if (ep_it != mount->options.error_pages.end()) {
                std::string ep = ep_it->second;
                if (!ep.empty() && ep.front() == '/') {
                    ep.erase(ep.begin());
                }
                std::string ep_path = mount->root;
                if (!ep.empty()) {
                    if (!ep_path.empty() && ep_path.back() != '/' && ep_path.back() != '\\') {
                        ep_path += '/';
                    }
                    ep_path += ep;
                }
                if (std::filesystem::exists(std::filesystem::path(std::u8string(ep_path.begin(), ep_path.end())))) {
                    serve_static_file(req, resp, *mount, ep, ep_path);
                    return;
                }
            }
            resp->process_error(ResponseCode::not_found);
            return;
        }

        serve_static_file(req, resp, *mount, file_relative_path, full_path);
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

        auto task = std::make_unique<net::http::HttpUploadFileTask>([resp, filePath]() {
            LOG_INFO("Download completed: {}", filePath);
            resp->set_upload_file(false);
        });

        const auto attachment_info = std::make_shared<net::http::AttachmentInfo>();
        attachment_info->origin_file_name_ = filePath;
        attachment_info->source_offset_ = 0;
        attachment_info->offset_ = 0;
        attachment_info->length_ = sz;
        task->set_attachment_info(attachment_info);

        if (!task->init()) {
            resp->process_error(ResponseCode::internal_server_error);
            return;
        }

        resp->add_header("Content-Type", get_content_type(ext));
        resp->add_header("Connection", "close");
        resp->set_response_code(ResponseCode::ok_);
        resp->add_header("Content-Length", std::to_string(sz));

        resp->set_task(task.release());
        resp->set_upload_file(true);
        resp->send();
    }

    void HttpServer::serve_list_files(const std::string & prefix, const std::string & filePath, const std::string & request_path, HttpResponse * resp, bool as_json)
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
        std::vector<nlohmann::json> entries;

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
                    item["url"] = request_path + (request_path.back() == '/' ? "" : "/") + entry.path().filename().string();
                } else if (entry.is_directory()) {
                    item["name"] = entry.path().filename().string();
                    item["type"] = 2;
                    item["modified"] = std::chrono::system_clock::to_time_t(sys_time);
                    item["url"] = request_path + (request_path.back() == '/' ? "" : "/") + entry.path().filename().string() + "/";
                }

                entries.push_back(std::move(item));
            }
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            resp->process_error();
            return;
        }

        std::sort(entries.begin(), entries.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
            const int type_a = a.value("type", 99);
            const int type_b = b.value("type", 99);
            if (type_a != type_b) {
                return type_a > type_b;
            }
            return a.value("name", std::string()) < b.value("name", std::string());
        });

        if (as_json) {
            for (auto &item : entries) {
                jsonResponse.push_back(std::move(item));
            }
            resp->json(jsonResponse.dump());
            resp->add_header("Connection", "close");
            resp->send();
            return;
        }

        std::string html;
        html += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
        html += "<title>Index of " + request_path + "</title>";
        html += "<style>body{font-family:Consolas,monospace;margin:24px}table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:8px;border-bottom:1px solid #ddd}a{text-decoration:none;color:#0a5}a:hover{text-decoration:underline}</style>";
        html += "</head><body><h1>Index of " + request_path + "</h1><table><thead><tr><th>Name</th><th>Size</th><th>Modified</th></tr></thead><tbody>";
        if (request_path != "/") {
            html += "<tr><td><a href=\"../\">../</a></td><td>-</td><td>-</td></tr>";
        }
        for (const auto &item : entries) {
            const bool is_dir = item.value("type", 0) == 2;
            const std::string name = item.value("name", "");
            const std::string url = item.value("url", "");
            const std::string size = is_dir ? "-" : std::to_string(item.value("size", static_cast<uint64_t>(0)));
            const auto modified = item.value("modified", static_cast<int64_t>(0));
            html += "<tr><td><a href=\"" + url + "\">" + name + (is_dir ? "/" : "") + "</a></td><td>" + size + "</td><td>" + std::to_string(modified) + "</td></tr>";
        }
        html += "</tbody></table></body></html>";
        resp->set_response_code(ResponseCode::ok_);
        resp->add_header("Content-Type", "text/html; charset=utf-8");
        resp->append_body(html);
        resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
        resp->send();
    }

    void HttpServer::reload_config(HttpRequest * req, HttpResponse * resp)
    {
        (void)req;
        if (HttpConfigManager::get_instance()->reload_config()) {
            load_static_paths();
            if (!init_proxy_if_needed()) {
                LOG_ERROR("reload proxy config failed");
            }
            refresh_h2_dispatch_paths();
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
        if (!is_safe_upload_id(upload_id)) {
            nlohmann::json err;
            err["error"] = "invalid uploadid";
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
            chunk_size = std::filesystem::file_size(std::filesystem::path(file_item->tmp_file), ec);
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
            session.touch(yuan::base::time::steady_now_ms());
            uploaded_chunks_[upload_id] = std::move(session);
            upload_session_count_.store(static_cast<uint32_t>(uploaded_chunks_.size()), std::memory_order_relaxed);
            session_it = uploaded_chunks_.find(upload_id);
            return true;
        }

        session_it->second.touch(yuan::base::time::steady_now_ms());

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
        if (create_dir_ec) {
            session.received.erase(chunk_index);
            nlohmann::json err;
            err["error"] = "failed to prepare upload temp directory";
            resp->json(err.dump(), ResponseCode::internal_server_error);
            resp->send();
            return false;
        }

        if (!file_item->is_in_memory()) {
            std::error_code copy_ec;
            std::filesystem::copy_file(
                std::filesystem::path(file_item->tmp_file),
                std::filesystem::path(chunk.tmp_path),
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
        } else {
            const char *data = file_item->data_begin;
            const auto data_size = file_item->data_end > file_item->data_begin
                ? static_cast<std::size_t>(file_item->data_end - file_item->data_begin)
                : 0;
            if (!data || data_size == 0) {
                session.received.erase(chunk_index);
                nlohmann::json err;
                err["error"] = "missing upload chunk data";
                resp->json(err.dump(), ResponseCode::bad_request);
                resp->send();
                return false;
            }

            std::ofstream out(std::filesystem::path(chunk.tmp_path), std::ios::binary | std::ios::trunc);
            if (!out.good()) {
                session.received.erase(chunk_index);
                nlohmann::json err;
                err["error"] = "failed to persist upload chunk";
                resp->json(err.dump(), ResponseCode::internal_server_error);
                resp->send();
                return false;
            }
            out.write(data, static_cast<std::streamsize>(data_size));
            if (!out.good()) {
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
        (void)req;
        (void)chunk_index;
        (void)file_item;
        const bool is_complete = session_snapshot.total_chunks > 0 && received_count == session_snapshot.total_chunks;

        if (is_complete) {
            auto task = std::make_unique<SaveUploadTempChunkTask>();
            task->set_session(std::make_shared<UploadSession>(session_snapshot));
            uploaded_chunks_.erase(session_snapshot.upload_id);
            upload_session_count_.store(static_cast<uint32_t>(uploaded_chunks_.size()), std::memory_order_relaxed);
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
        UploadSession session_snapshot;
        int received_count = 0;
        {
            std::lock_guard<std::mutex> upload_lock(upload_mutex_);
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
        }

        nlohmann::json result;
        result["uploaded"] = true;
        result["chunkIdx"] = chunk_index;
        result["received"] = received_count;
        result["total"] = session_snapshot.total_chunks;
        resp->json(result.dump(), ResponseCode::ok_);
        resp->send();
    }
}
