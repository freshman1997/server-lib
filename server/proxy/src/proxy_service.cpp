#include "proxy_service.h"

#include "base/utils/base64.h"
#include "coroutine/completion_event.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/io_result.h"
#include "coroutine/task.h"
#include "logger.h"
#include "net/async/async_connection_context.h"
#include "net/async/async_listener_host.h"
#include "net/connection/connection.h"
#include "net/connection/connection_handle.h"
#include "net/auth_rate_limiter.h"
#include "net/ip_policy.h"
#include "net/runtime/network_runtime.h"
#include "server_service_custom_events.h"
#include "timer/timer_handle.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace
{
    constexpr std::size_t kProxyStreamReadBufferBytes = 64 * 1024;
    constexpr std::size_t kResponseProbeMaxBytes = 64 * 1024;

    void tune_proxy_stream_connection(const std::shared_ptr<yuan::net::Connection> &connection)
    {
        if (connection) {
            connection->set_max_packet_size(kProxyStreamReadBufferBytes);
        }
    }

    uint64_t current_process_working_set_bytes()
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc))) {
            return static_cast<uint64_t>(pmc.WorkingSetSize);
        }
        return 0;
#elif defined(__linux__)
        long rss_pages = 0;
        FILE *fp = std::fopen("/proc/self/statm", "r");
        if (!fp) {
            return 0;
        }
        if (std::fscanf(fp, "%*s %ld", &rss_pages) != 1) {
            std::fclose(fp);
            return 0;
        }
        std::fclose(fp);
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0 || rss_pages <= 0) {
            return 0;
        }
        return static_cast<uint64_t>(rss_pages) * static_cast<uint64_t>(page_size);
#elif defined(__APPLE__)
        mach_task_basic_info info;
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
            return static_cast<uint64_t>(info.resident_size);
        }
        return 0;
#else
        return 0;
#endif
    }

    std::string trim(std::string value)
    {
        std::size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
            ++begin;
        }
        std::size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    std::string to_lower(std::string value)
    {
        for (char &ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    bool wildcard_match(std::string_view pattern, std::string_view value)
    {
        std::size_t p = 0;
        std::size_t v = 0;
        std::size_t star = std::string_view::npos;
        std::size_t match = 0;

        while (v < value.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
                ++p;
                ++v;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                match = v;
            } else if (star != std::string_view::npos) {
                p = star + 1;
                v = ++match;
            } else {
                return false;
            }
        }

        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }
        return p == pattern.size();
    }

    bool match_rule(const std::string &rule, const std::string &host, uint16_t port)
    {
        const auto colon = rule.rfind(':');
        std::string host_rule = to_lower(trim(rule));
        std::string port_rule = "*";
        if (colon != std::string::npos && rule.find(':') == colon) {
            host_rule = to_lower(trim(rule.substr(0, colon)));
            port_rule = trim(rule.substr(colon + 1));
        }
        const bool host_ok = wildcard_match(host_rule.empty() ? "*" : host_rule, to_lower(host));
        const bool port_ok = port_rule == "*" || (!port_rule.empty() && static_cast<uint16_t>(std::stoi(port_rule)) == port);
        return host_ok && port_ok;
    }

    bool target_allowed(const yuan::server::ProxyServiceConfig &config, const std::string &host, uint16_t port)
    {
        for (const auto &rule : config.deny_targets) {
            if (!rule.empty() && match_rule(rule, host, port)) {
                return false;
            }
        }
        if (config.allow_targets.empty()) {
            return true;
        }
        for (const auto &rule : config.allow_targets) {
            if (!rule.empty() && match_rule(rule, host, port)) {
                return true;
            }
        }
        return false;
    }

    std::unordered_map<std::string, std::string> parse_header_map(const std::string &request)
    {
        std::unordered_map<std::string, std::string> headers;
        const auto header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return headers;
        }

        std::size_t line_start = request.find("\r\n");
        if (line_start == std::string::npos) {
            return headers;
        }
        line_start += 2;

        while (line_start < header_end) {
            const auto line_end = request.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end > header_end) {
                break;
            }
            const auto colon = request.find(':', line_start);
            if (colon != std::string::npos && colon < line_end) {
                auto name = to_lower(trim(request.substr(line_start, colon - line_start)));
                auto value = trim(request.substr(colon + 1, line_end - colon - 1));
                if (!name.empty()) {
                    headers[name] = std::move(value);
                }
            }
            line_start = line_end + 2;
        }

        return headers;
    }

    bool verify_basic_proxy_auth(const std::unordered_map<std::string, std::string> &headers,
                                 const yuan::server::ProxyServiceConfig &config)
    {
        if (config.basic_auth_user.empty()) {
            return true;
        }

        const auto it = headers.find("proxy-authorization");
        if (it == headers.end()) {
            return false;
        }

        const std::string &value = it->second;
        if (value.size() < 6 || to_lower(value.substr(0, 6)) != "basic ") {
            return false;
        }

        const std::string decoded = yuan::base::util::base64_decode(value.substr(6));
        const auto sep = decoded.find(':');
        if (sep == std::string::npos) {
            return false;
        }

        return decoded.substr(0, sep) == config.basic_auth_user &&
               decoded.substr(sep + 1) == config.basic_auth_password;
    }

    struct ConnectTarget
    {
        std::string host;
        uint16_t port = 443;
        std::string request_path = "/";
    };

    bool parse_connect_target(const std::string &target, ConnectTarget &out)
    {
        if (target.empty()) {
            return false;
        }

        if (target.front() == '[') {
            const auto close = target.find(']');
            if (close == std::string::npos || close + 2 >= target.size() || target[close + 1] != ':') {
                return false;
            }
            out.host = target.substr(1, close - 1);
            out.port = static_cast<uint16_t>(std::stoi(target.substr(close + 2)));
            return true;
        }

        const auto colon = target.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size()) {
            return false;
        }
        if (target.find(':') != colon) {
            return false;
        }

        out.host = target.substr(0, colon);
        out.port = static_cast<uint16_t>(std::stoi(target.substr(colon + 1)));
        return true;
    }

    bool parse_forward_target(const std::string &target,
                              const std::unordered_map<std::string, std::string> &headers,
                              ConnectTarget &out)
    {
        if (target.empty()) {
            return false;
        }

        auto parse_host_port = [](const std::string &host_port, uint16_t default_port, ConnectTarget &dst) -> bool {
            if (host_port.empty()) {
                return false;
            }
            if (host_port.front() == '[') {
                const auto close = host_port.find(']');
                if (close == std::string::npos) {
                    return false;
                }
                dst.host = host_port.substr(1, close - 1);
                if (close + 1 < host_port.size()) {
                    if (host_port[close + 1] != ':' || close + 2 >= host_port.size()) {
                        return false;
                    }
                    dst.port = static_cast<uint16_t>(std::stoi(host_port.substr(close + 2)));
                } else {
                    dst.port = default_port;
                }
                return true;
            }

            const auto colon = host_port.rfind(':');
            if (colon != std::string::npos && host_port.find(':') == colon) {
                dst.host = host_port.substr(0, colon);
                dst.port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
                return !dst.host.empty();
            }

            dst.host = host_port;
            dst.port = default_port;
            return !dst.host.empty();
        };

        if (target[0] == '/') {
            const auto host_it = headers.find("host");
            if (host_it == headers.end() || !parse_host_port(trim(host_it->second), 80, out)) {
                return false;
            }
            out.request_path = target;
            return true;
        }

        const auto scheme_pos = target.find("://");
        if (scheme_pos == std::string::npos) {
            return false;
        }

        const std::string scheme = to_lower(target.substr(0, scheme_pos));
        const std::size_t authority_start = scheme_pos + 3;
        const std::size_t path_pos = target.find('/', authority_start);
        const std::string authority = path_pos == std::string::npos
            ? target.substr(authority_start)
            : target.substr(authority_start, path_pos - authority_start);

        const uint16_t default_port = scheme == "https" ? 443 : 80;
        if (!parse_host_port(authority, default_port, out)) {
            return false;
        }

        out.request_path = path_pos == std::string::npos ? "/" : target.substr(path_pos);
        return true;
    }

    std::string build_forward_request(const std::string &method,
                                      const std::string &version,
                                      const std::string &request_path,
                                      const std::string &request,
                                      const std::unordered_map<std::string, std::string> &headers)
    {
        const auto header_end = request.find("\r\n\r\n");
        const auto first_line_end = request.find("\r\n");
        std::ostringstream oss;
        oss << method << ' ' << request_path << ' ' << (version.empty() ? "HTTP/1.1" : version) << "\r\n";

        std::size_t line_start = first_line_end == std::string::npos ? std::string::npos : first_line_end + 2;
        while (line_start != std::string::npos && header_end != std::string::npos && line_start < header_end) {
            const auto line_end = request.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end > header_end) {
                break;
            }
            const auto colon = request.find(':', line_start);
            if (colon == std::string::npos || colon >= line_end) {
                line_start = line_end + 2;
                continue;
            }

            const std::string name = trim(request.substr(line_start, colon - line_start));
            const std::string lower_name = to_lower(name);
            if (lower_name == "proxy-authorization" || lower_name == "proxy-connection") {
                line_start = line_end + 2;
                continue;
            }
            if (lower_name == "connection") {
                oss << "Connection: close\r\n";
                line_start = line_end + 2;
                continue;
            }

            oss << request.substr(line_start, line_end - line_start) << "\r\n";
            line_start = line_end + 2;
        }

        if (headers.find("connection") == headers.end()) {
            oss << "Connection: close\r\n";
        }
        oss << "\r\n";
        return oss.str();
    }

    struct SessionGuard
    {
        explicit SessionGuard(std::atomic_int &counter) : counter_(counter)
        {
            counter_.fetch_add(1, std::memory_order_relaxed);
        }

        ~SessionGuard()
        {
            counter_.fetch_sub(1, std::memory_order_relaxed);
        }

        std::atomic_int &counter_;
    };

    struct ScopeExit
    {
        std::function<void()> fn;

        ~ScopeExit()
        {
            if (fn) {
                fn();
            }
        }
    };

    struct RelaySharedState
    {
        yuan::coroutine::RuntimeView runtime;
        yuan::net::ConnectionHandle client_connection;
        yuan::net::ConnectionHandle upstream_connection;
        std::string client_addr;
        std::string upstream_addr;
        std::string method;
        std::string target_host;
        int target_port = 0;
        std::atomic_bool closed{ false };
        std::atomic_int remaining_relays{ 0 };
        std::atomic_uint64_t bytes_up{ 0 };
        std::atomic_uint64_t bytes_down{ 0 };
        std::atomic_uint64_t pending_buffer_bytes{ 0 };
        std::atomic_uint64_t *total_tunnel_memory{ nullptr };
        int max_session_buffer_bytes = 0;
        int max_total_tunnel_memory = 0;
        yuan::coroutine::CompletionEvent relays_completed;
        std::mutex reason_mutex;
        std::string close_reason = "closed";
        std::function<void(bool client_half_closed)> on_half_close;
    };

    enum class ProxySessionState {
        accepted,
        reading_request,
        connecting_upstream,
        established,
        half_closed_client,
        half_closed_upstream,
        closing,
        closed,
    };

    const char *proxy_session_state_name(ProxySessionState state) noexcept
    {
        switch (state) {
        case ProxySessionState::accepted:
            return "accepted";
        case ProxySessionState::reading_request:
            return "reading_request";
        case ProxySessionState::connecting_upstream:
            return "connecting_upstream";
        case ProxySessionState::established:
            return "established";
        case ProxySessionState::half_closed_client:
            return "half_closed_client";
        case ProxySessionState::half_closed_upstream:
            return "half_closed_upstream";
        case ProxySessionState::closing:
            return "closing";
        case ProxySessionState::closed:
            return "closed";
        }
        return "unknown";
    }

    std::string format_remote_address(const yuan::net::InetAddress &address)
    {
        return address.to_address_key();
    }

    std::string format_client_identity(const yuan::net::InetAddress &address)
    {
        if (!address.get_ip().empty()) {
            return address.get_ip();
        }
        return address.to_address_key();
    }

    std::string io_status_reason(yuan::coroutine::IoStatus status,
                                 std::string_view closed_reason,
                                 std::string_view error_reason,
                                 std::string_view timeout_reason)
    {
        switch (status) {
        case yuan::coroutine::IoStatus::timed_out:
            return std::string(timeout_reason);
        case yuan::coroutine::IoStatus::connection_closed:
            return std::string(closed_reason);
        case yuan::coroutine::IoStatus::success:
            return "closed";
        case yuan::coroutine::IoStatus::connection_error:
        case yuan::coroutine::IoStatus::invalid_state:
        default:
            return std::string(error_reason);
        }
    }

    int proxy_http_status_for_failure(const std::string &reason,
                                      bool is_connect_method,
                                      uint64_t bytes_down)
    {
        if (is_connect_method) {
            return 0;
        }

        if (reason == "idle_timeout") {
            return 504;
        }
        if (reason == "upstream_no_response") {
            return 504;
        }
        if (reason == "buffer_overflow" || reason == "total_memory_overflow") {
            return 503;
        }
        if (reason == "upstream_error" || reason == "upstream_write_failed" || reason == "upstream_closed") {
            return bytes_down == 0 ? 502 : 0;
        }
        if (reason == "upstream_write_timeout") {
            return 504;
        }
        return 0;
    }

    void increment_close_reason_counter(yuan::server::ProxyServiceMetrics *metrics, const std::string &reason)
    {
        if (!metrics) {
            return;
        }
        if (reason == "idle_timeout") {
            metrics->idle_timeouts.fetch_add(1, std::memory_order_relaxed);
        } else if (reason == "client_closed" || reason == "client_error") {
            metrics->closes_by_client.fetch_add(1, std::memory_order_relaxed);
        } else if (reason == "upstream_closed" || reason == "upstream_error") {
            metrics->closes_by_upstream.fetch_add(1, std::memory_order_relaxed);
        } else if (reason == "ssrf_blocked") {
            metrics->closes_by_ssrf.fetch_add(1, std::memory_order_relaxed);
        } else if (reason == "acl_denied") {
            metrics->closes_by_acl.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void close_relay_state(const std::shared_ptr<RelaySharedState> &state, const std::string &reason)
    {
        bool expected = false;
        if (!state || !state->closed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state->reason_mutex);
            state->close_reason = reason;
        }

        if (state->client_connection) {
            state->client_connection->close();
        }
        if (state->upstream_connection) {
            state->upstream_connection->close();
        }
    }

    void note_relay_reason(const std::shared_ptr<RelaySharedState> &state, const std::string &reason)
    {
        if (!state) {
            return;
        }
        std::lock_guard<std::mutex> lock(state->reason_mutex);
        if (state->close_reason == "closed") {
            state->close_reason = reason;
        }
    }

    yuan::coroutine::Task<bool> write_buffer_async(yuan::net::AsyncConnectionContext &ctx,
                                                   const ::yuan::buffer::ByteBuffer &buffer,
                                                   uint32_t timeout_ms = 0)
    {
        auto wr = co_await ctx.write_async(buffer, timeout_ms);
        co_return wr.status == yuan::coroutine::IoStatus::success;
    }

    yuan::coroutine::Task<bool> write_text_async(yuan::net::AsyncConnectionContext &ctx,
                                                 const std::string &text,
                                                 uint32_t timeout_ms = 0)
    {
        ::yuan::buffer::ByteBuffer buffer(text.size());
        buffer.append(std::string_view(text));
        co_return co_await write_buffer_async(ctx, buffer, timeout_ms);
    }

    struct AsyncHttpReadResult
    {
        bool ok = false;
        bool too_large = false;
        std::string request;
        yuan::coroutine::IoStatus status = yuan::coroutine::IoStatus::success;
    };

    yuan::coroutine::Task<AsyncHttpReadResult> read_http_request_async(yuan::net::AsyncConnectionContext &ctx,
                                                                       std::size_t max_header_bytes,
                                                                       uint32_t timeout_ms)
    {
        AsyncHttpReadResult result;
        while (result.request.size() < max_header_bytes) {
            auto read_result = co_await ctx.read_async(timeout_ms);
            if (read_result.status != yuan::coroutine::IoStatus::success) {
                result.status = read_result.status;
                co_return result;
            }

            const auto span = read_result.data.readable_span();
            result.request.append(span.data(), span.size());
            if (result.request.find("\r\n\r\n") != std::string::npos) {
                result.ok = true;
                co_return result;
            }
        }

        result.too_large = true;
        co_return result;
    }

    yuan::coroutine::Task<bool> write_http_text_async(yuan::net::AsyncConnectionContext &ctx,
                                                      int status_code,
                                                      const std::string &reason,
                                                      const std::string &body = {})
    {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status_code << ' ' << reason << "\r\n"
            << "Proxy-Agent: yuan-proxy-service\r\n"
            << "Connection: close\r\n";
        if (!body.empty()) {
            oss << "Content-Type: text/plain; charset=utf-8\r\n"
                << "Content-Length: " << body.size() << "\r\n";
        } else {
            oss << "Content-Length: 0\r\n";
        }
        oss << "\r\n";
        if (!body.empty()) {
            oss << body;
        }
        co_return co_await write_text_async(ctx, oss.str());
    }

    yuan::coroutine::Task<void> relay_one_way_async(const std::shared_ptr<RelaySharedState> &state,
                                                    bool client_to_upstream,
                                                    uint32_t idle_timeout_ms)
    {
        ScopeExit finish{ [state]() {
            if (state && state->remaining_relays.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                state->relays_completed.notify();
            }
        } };

        auto &runtime = state->runtime;
        auto &src_handle = client_to_upstream ? state->client_connection : state->upstream_connection;
        auto &dst_handle = client_to_upstream ? state->upstream_connection : state->client_connection;
        if (!src_handle || !dst_handle) {
            co_return;
        }
        yuan::net::Connection *src = src_handle.get();
        yuan::net::Connection *dst = dst_handle.get();
        const bool plain_http_downstream = !client_to_upstream && state->method != "CONNECT";

        while (!state->closed.load(std::memory_order_acquire) && src->is_connected() && dst->is_connected()) {
            auto read_result = co_await runtime.read(src_handle, idle_timeout_ms);
            const bool peer_input_shutdown =
                read_result.status == yuan::coroutine::IoStatus::connection_closed &&
                src && src->input_shutdown();
            if (peer_input_shutdown) {
                LOG_DEBUG("[ProxyService] session relay {}->{} target={}:{} client={} upstream={} peer input shutdown, forwarding FIN",
                          client_to_upstream ? "client" : "upstream",
                          client_to_upstream ? "upstream" : "client",
                          state->target_host,
                          state->target_port,
                          state->client_addr,
                          state->upstream_addr);
                note_relay_reason(state, client_to_upstream ? "client_closed" : "upstream_closed");
                if (state->on_half_close) {
                    state->on_half_close(client_to_upstream);
                }
                if (dst) {
                    (void)dst->shutdown_write();
                }
                if (plain_http_downstream) {
                    close_relay_state(state, "upstream_closed");
                }
                co_return;
            }

            if (read_result.status == yuan::coroutine::IoStatus::connection_error) {
                auto ctx = yuan::net::AsyncConnectionContext(src->shared_from_this(), runtime);
                auto shutdown_result = co_await ctx.read_async(idle_timeout_ms, false);
                if (shutdown_result.status == yuan::coroutine::IoStatus::connection_closed && src->input_shutdown()) {
                    LOG_DEBUG("[ProxyService] session relay {}->{} target={}:{} client={} upstream={} shutdown observed after read race, forwarding FIN",
                              client_to_upstream ? "client" : "upstream",
                              client_to_upstream ? "upstream" : "client",
                              state->target_host,
                              state->target_port,
                              state->client_addr,
                              state->upstream_addr);
                    note_relay_reason(state, client_to_upstream ? "client_closed" : "upstream_closed");
                    if (state->on_half_close) {
                        state->on_half_close(client_to_upstream);
                    }
                    if (dst) {
                        (void)dst->shutdown_write();
                    }
                    if (plain_http_downstream) {
                        close_relay_state(state, "upstream_closed");
                    }
                    co_return;
                }
                read_result = std::move(shutdown_result);
            }

            if (read_result.status != yuan::coroutine::IoStatus::success) {
                LOG_DEBUG("[ProxyService] session relay {}->{} target={}:{} client={} upstream={} read failed status={}, will close",
                          client_to_upstream ? "client" : "upstream",
                          client_to_upstream ? "upstream" : "client",
                          state->target_host,
                          state->target_port,
                          state->client_addr,
                          state->upstream_addr,
                          static_cast<int>(read_result.status));
                close_relay_state(state,
                                  io_status_reason(read_result.status,
                                                   client_to_upstream ? "client_closed" : "upstream_closed",
                                                   client_to_upstream ? "client_error" : "upstream_error",
                                                   "idle_timeout"));
                co_return;
            }

            const auto bytes = static_cast<uint64_t>(read_result.data.readable_bytes());
            if (bytes == 0) {
                LOG_DEBUG("[ProxyService] session relay {}->{} target={}:{} client={} upstream={} read returned 0 bytes, connection closed by peer",
                          client_to_upstream ? "client" : "upstream",
                          client_to_upstream ? "upstream" : "client",
                          state->target_host,
                          state->target_port,
                          state->client_addr,
                          state->upstream_addr);
                note_relay_reason(state, client_to_upstream ? "client_closed" : "upstream_closed");
                if (state->on_half_close) {
                    state->on_half_close(client_to_upstream);
                }
                if (dst) {
                    (void)dst->shutdown_write();
                }
                if (plain_http_downstream) {
                    close_relay_state(state, "upstream_closed");
                }
                co_return;
            }

            if (state->max_session_buffer_bytes > 0) {
                const uint64_t current_pending = state->pending_buffer_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
                if (current_pending > static_cast<uint64_t>(state->max_session_buffer_bytes)) {
                    state->pending_buffer_bytes.fetch_sub(bytes, std::memory_order_relaxed);
                    LOG_WARN("[ProxyService] session relay {}->{} target={}:{} buffer overflow: pending={} max={}",
                             client_to_upstream ? "client" : "upstream",
                             client_to_upstream ? "upstream" : "client",
                             state->target_host,
                             state->target_port,
                             current_pending,
                             state->max_session_buffer_bytes);
                    close_relay_state(state, "buffer_overflow");
                    co_return;
                }
            }

            if (state->max_total_tunnel_memory > 0 && state->total_tunnel_memory) {
                const uint64_t current_total = state->total_tunnel_memory->fetch_add(bytes, std::memory_order_relaxed) + bytes;
                if (current_total > static_cast<uint64_t>(state->max_total_tunnel_memory)) {
                    state->total_tunnel_memory->fetch_sub(bytes, std::memory_order_relaxed);
                    LOG_WARN("[ProxyService] session relay {}->{} target={}:{} total tunnel memory overflow: total={} max={}",
                             client_to_upstream ? "client" : "upstream",
                             client_to_upstream ? "upstream" : "client",
                             state->target_host,
                             state->target_port,
                             current_total,
                             state->max_total_tunnel_memory);
                    close_relay_state(state, "total_memory_overflow");
                    co_return;
                }
            }

            auto wr = co_await runtime.write(dst_handle, read_result.data, idle_timeout_ms);
            if (state->max_session_buffer_bytes > 0) {
                state->pending_buffer_bytes.fetch_sub(bytes, std::memory_order_relaxed);
            }
            if (state->max_total_tunnel_memory > 0 && state->total_tunnel_memory) {
                state->total_tunnel_memory->fetch_sub(bytes, std::memory_order_relaxed);
            }
            if (wr.status == yuan::coroutine::IoStatus::connection_closed && dst && dst->input_shutdown()) {
                LOG_DEBUG("[ProxyService] session relay {}->{} target={}:{} client={} upstream={} peer closed after successful half-close relay",
                          client_to_upstream ? "upstream" : "client",
                          client_to_upstream ? "client" : "upstream",
                          state->target_host,
                          state->target_port,
                          state->client_addr,
                          state->upstream_addr);
                note_relay_reason(state, client_to_upstream ? "upstream_closed" : "client_closed");
                co_return;
            }
            if (wr.status != yuan::coroutine::IoStatus::success) {
                LOG_DEBUG("[ProxyService] session relay {}->{} target={}:{} client={} upstream={} write failed status={}",
                          client_to_upstream ? "upstream" : "client",
                          client_to_upstream ? "client" : "upstream",
                          state->target_host,
                          state->target_port,
                          state->client_addr,
                          state->upstream_addr,
                          static_cast<int>(wr.status));
                close_relay_state(state,
                                  io_status_reason(wr.status,
                                                   client_to_upstream ? "upstream_closed" : "client_closed",
                                                   client_to_upstream ? "upstream_write_failed" : "client_write_failed",
                                                   client_to_upstream ? "upstream_write_timeout" : "client_write_timeout"));
                co_return;
            }

            if (client_to_upstream) {
                state->bytes_up.fetch_add(bytes, std::memory_order_relaxed);
            } else {
                state->bytes_down.fetch_add(bytes, std::memory_order_relaxed);
            }
        }
    }

    yuan::coroutine::Task<void> handle_http_proxy_client_async(
        uint64_t session_id,
        yuan::net::AsyncConnectionContext ctx,
        yuan::coroutine::RuntimeView runtime,
        const yuan::server::ProxyServiceConfig &config,
        std::atomic_int &active_sessions,
        yuan::server::ServerRuntimeHost *host,
        std::atomic_uint64_t *completed_sessions,
        yuan::server::ProxyServiceMetrics *metrics,
        yuan::net::AuthRateLimiter *auth_rate_limiter,
        std::function<void(std::string_view, std::string_view, std::string_view, std::string_view, int)> on_metadata,
        std::function<void(ProxySessionState, std::string_view)> on_state_change,
        std::function<void(const std::shared_ptr<yuan::net::Connection> &)> on_upstream_ready,
        std::function<void(const std::shared_ptr<RelaySharedState> &)> on_relay_ready,
        std::atomic_uint64_t *total_tunnel_memory,
        std::function<void()> on_finish)
    {
        SessionGuard guard(active_sessions);
        ScopeExit finish{ on_finish };
        const auto started_at = std::chrono::steady_clock::now();
        const std::string peer_text = format_remote_address(ctx.get_remote_address());
        std::chrono::steady_clock::time_point request_read_completed_at = started_at;

        if (auth_rate_limiter && !config.basic_auth_user.empty() && auth_rate_limiter->is_banned(peer_text)) {
            LOG_WARN("[ProxyService] session #{} rejecting banned client {} due to auth rate limit", session_id, peer_text);
            (void)co_await write_http_text_async(ctx, 429, "Too Many Requests", "auth rate limit exceeded");
            ctx.close();
            co_return;
        }
        std::chrono::steady_clock::time_point upstream_connected_at = started_at;
        bool lifecycle_completed = false;
        ScopeExit state_finish{ [&]() {
            if (lifecycle_completed || !on_state_change) {
                return;
            }
            on_state_change(ProxySessionState::closing, "session_exiting");
            on_state_change(ProxySessionState::closed, "session_exiting");
        } };

        try {
            LOG_INFO("[ProxyService] session #{} client connected from {}", session_id, peer_text);
            LOG_INFO("[ProxyService] session #{} entering request handling", session_id);
            if (on_state_change) {
                LOG_INFO("[ProxyService] session #{} invoking reading_request state transition", session_id);
                on_state_change(ProxySessionState::reading_request, "request_started");
                LOG_INFO("[ProxyService] session #{} reading_request state transition finished", session_id);
            }

            auto request_result = co_await read_http_request_async(
                ctx,
                static_cast<std::size_t>(std::max(config.max_header_bytes, 1)),
                static_cast<uint32_t>(config.header_timeout_ms));
            LOG_INFO("[ProxyService] session #{} request read completed ok={} too_large={} status={} bytes={}",
                     session_id,
                     request_result.ok,
                     request_result.too_large,
                     static_cast<int>(request_result.status),
                     request_result.request.size());
            request_read_completed_at = std::chrono::steady_clock::now();
            if (!request_result.ok) {
                if (request_result.too_large) {
                    LOG_WARN("[ProxyService] session #{} {} request header too large", session_id, peer_text);
                    (void)co_await write_http_text_async(ctx, 431, "Request Header Fields Too Large");
                } else if (request_result.status == yuan::coroutine::IoStatus::timed_out) {
                    LOG_WARN("[ProxyService] session #{} {} request header timeout", session_id, peer_text);
                    if (metrics) {
                        metrics->header_timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
                    (void)co_await write_http_text_async(ctx, 408, "Request Timeout");
                } else {
                    LOG_WARN("[ProxyService] session #{} {} failed to read request, status={}",
                             session_id, peer_text, static_cast<int>(request_result.status));
                }
                ctx.close();
                co_return;
            }

            const std::string &request = request_result.request;
            const auto line_end = request.find("\r\n");
            const auto header_end = request.find("\r\n\r\n");
            if (line_end == std::string::npos) {
                LOG_WARN("[ProxyService] session #{} {} malformed request line", session_id, peer_text);
                (void)co_await write_http_text_async(ctx, 400, "Bad Request");
                ctx.close();
                co_return;
            }

            const auto headers = parse_header_map(request);
            if (!verify_basic_proxy_auth(headers, config)) {
                if (auth_rate_limiter) {
                    auth_rate_limiter->record_failure(peer_text);
                }
                const std::string response =
                    "HTTP/1.1 407 Proxy Authentication Required\r\n"
                    "Proxy-Agent: yuan-proxy-service\r\n"
                    "Proxy-Authenticate: Basic realm=\"yuan-proxy\"\r\n"
                    "Connection: close\r\n"
                    "Content-Length: 0\r\n\r\n";
                (void)co_await write_text_async(ctx, response);
                ctx.close();
                co_return;
            }

            if (auth_rate_limiter && !config.basic_auth_user.empty()) {
                auth_rate_limiter->record_success(peer_text);
            }

            std::istringstream iss(request.substr(0, line_end));
            std::string method;
            std::string target;
            std::string version;
            iss >> method >> target >> version;
            if (method.empty() || target.empty()) {
                LOG_WARN("[ProxyService] session #{} {} incomplete request line", session_id, peer_text);
                (void)co_await write_http_text_async(ctx, 400, "Bad Request");
                ctx.close();
                co_return;
            }

            ConnectTarget connect_target;
            if (method == "CONNECT") {
                if (!parse_connect_target(target, connect_target)) {
                    LOG_WARN("[ProxyService] session #{} {} invalid CONNECT target {}", session_id, peer_text, target);
                    (void)co_await write_http_text_async(ctx, 400, "Bad Request", "invalid CONNECT target");
                    ctx.close();
                    co_return;
                }
            } else {
                if (!parse_forward_target(target, headers, connect_target)) {
                    LOG_WARN("[ProxyService] session #{} {} invalid forward target {}", session_id, peer_text, target);
                    (void)co_await write_http_text_async(ctx, 400, "Bad Request", "invalid proxy target");
                    ctx.close();
                    co_return;
                }
            }

            std::string leftover;
            if (header_end != std::string::npos) {
                leftover = request.substr(header_end + 4);
            }

            if (on_metadata) {
                on_metadata(peer_text,
                            method,
                            connect_target.host + ":" + std::to_string(connect_target.port),
                            connect_target.host,
                            connect_target.port);
            }

            LOG_INFO("[ProxyService] session #{} request parsed method={} version={} client={} target={} host={} port={} path={}",
                     session_id,
                     method,
                     version,
                     peer_text,
                     connect_target.host + ":" + std::to_string(connect_target.port),
                     connect_target.host,
                     connect_target.port,
                     connect_target.request_path.empty() ? "/" : connect_target.request_path);

            if (!target_allowed(config, connect_target.host, connect_target.port)) {
                LOG_WARN("[ProxyService] session #{} {} target denied {}:{}",
                         session_id, peer_text, connect_target.host, connect_target.port);
                if (metrics) {
                    metrics->closes_by_acl.fetch_add(1, std::memory_order_relaxed);
                }
                if (host) {
                    yuan::server::ProxySessionRejectedEvent evt;
                    evt.session_id = session_id;
                    evt.service_name = "proxy";
                    evt.client_addr = peer_text;
                    evt.method = method;
                    evt.target_addr = connect_target.host + ":" + std::to_string(connect_target.port);
                    evt.reason = "acl_denied";
                    host->publish_custom(yuan::server::events::proxy_session_rejected, std::move(evt));
                }
                (void)co_await write_http_text_async(ctx, 403, "Forbidden", "target denied by proxy policy");
                ctx.close();
                co_return;
            }

            if (on_state_change) {
                on_state_change(ProxySessionState::connecting_upstream, "upstream_connect_started");
            }

            auto connect_result = co_await yuan::coroutine::async_connect(runtime,
                                                                          connect_target.host,
                                                                          connect_target.port,
                                                                          static_cast<uint32_t>(config.connect_timeout_ms));
            if (connect_result.result != yuan::coroutine::ConnectResult::success || !connect_result.connection) {
                LOG_ERROR("[ProxyService] session #{} {} failed to connect upstream {}:{} result={}",
                          session_id, peer_text, connect_target.host, connect_target.port, static_cast<int>(connect_result.result));
                if (connect_result.result == yuan::coroutine::ConnectResult::timed_out) {
                    if (metrics) {
                        metrics->connect_timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
                    (void)co_await write_http_text_async(ctx, 504, "Gateway Timeout", "upstream connect timed out");
                } else {
                    (void)co_await write_http_text_async(ctx, 502, "Bad Gateway", "failed to connect upstream");
                }
                ctx.close();
                co_return;
            }

            tune_proxy_stream_connection(connect_result.connection);

            if (on_upstream_ready) {
                on_upstream_ready(connect_result.connection);
            }
            upstream_connected_at = std::chrono::steady_clock::now();
            const std::string upstream_peer_text = format_remote_address(connect_result.connection->get_remote_address());
            LOG_INFO("[ProxyService] session #{} upstream connected client={} target={} upstream_peer={} method={}",
                     session_id,
                     peer_text,
                     connect_target.host + ":" + std::to_string(connect_target.port),
                     upstream_peer_text,
                     method);

            if (!config.allow_private_targets) {
                const std::string resolved_ip = connect_result.connection->get_remote_address().get_ip();
                if (yuan::net::is_private_ip(resolved_ip)) {
                    LOG_WARN("[ProxyService] session #{} SSRF blocked, target {} resolves to private IP {}",
                             session_id, connect_target.host, resolved_ip);
                    if (metrics) {
                        metrics->closes_by_ssrf.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (host) {
                        yuan::server::ProxySessionRejectedEvent evt;
                        evt.session_id = session_id;
                        evt.service_name = "proxy";
                        evt.client_addr = peer_text;
                        evt.method = method;
                        evt.target_addr = connect_target.host + ":" + std::to_string(connect_target.port);
                        evt.reason = "ssrf_blocked";
                        host->publish_custom(yuan::server::events::proxy_session_rejected, std::move(evt));
                    }
                    (void)co_await write_http_text_async(ctx, 403, "Forbidden", "target resolves to private IP");
                    connect_result.connection->close();
                    ctx.close();
                    co_return;
                }
            }

            yuan::net::AsyncConnectionContext upstream_ctx(connect_result.connection, runtime);

            if (host) {
                yuan::server::ProxySessionAcceptedEvent evt;
                evt.session_id = session_id;
                evt.service_name = "proxy";
                evt.client_addr = peer_text;
                evt.method = method;
                evt.target_addr = connect_target.host + ":" + std::to_string(connect_target.port);
                evt.active_sessions = static_cast<uint32_t>(active_sessions.load(std::memory_order_relaxed));
                host->publish_custom(yuan::server::events::proxy_session_accepted, std::move(evt));
            }

            if (on_state_change) {
                on_state_change(ProxySessionState::established, method == "CONNECT" ? "connect_established" : "forward_established");
            }

        uint64_t bytes_up = 0;
        uint64_t bytes_down = 0;
        std::string close_reason = "upstream_closed";

        if (method == "CONNECT") {
            const std::string established =
                "HTTP/1.1 200 Connection Established\r\n"
                "Proxy-Agent: yuan-proxy-service\r\n"
                "\r\n";
            if (!co_await write_text_async(ctx, established, static_cast<uint32_t>(config.idle_timeout_ms))) {
                LOG_ERROR("[ProxyService] session #{} {} failed to write CONNECT response", session_id, peer_text);
                ctx.close();
                upstream_ctx.close();
                co_return;
            }

            if (!leftover.empty()) {
                ::yuan::buffer::ByteBuffer leftover_buffer(leftover.size());
                leftover_buffer.append(std::string_view(leftover));
                if (!co_await write_buffer_async(upstream_ctx, leftover_buffer, static_cast<uint32_t>(config.idle_timeout_ms))) {
                    LOG_ERROR("[ProxyService] session #{} {} failed to forward CONNECT leftover bytes", session_id, peer_text);
                    ctx.close();
                    upstream_ctx.close();
                    co_return;
                }
            }

        } else {
            const std::string forward_request = build_forward_request(method, version, connect_target.request_path, request, headers);
            if (!co_await write_text_async(upstream_ctx, forward_request, static_cast<uint32_t>(config.idle_timeout_ms))) {
                LOG_ERROR("[ProxyService] session #{} {} failed to forward request head", session_id, peer_text);
                ctx.close();
                upstream_ctx.close();
                co_return;
            }

            if (!leftover.empty()) {
                ::yuan::buffer::ByteBuffer leftover_buffer(leftover.size());
                leftover_buffer.append(std::string_view(leftover));
                if (!co_await write_buffer_async(upstream_ctx, leftover_buffer, static_cast<uint32_t>(config.idle_timeout_ms))) {
                    LOG_ERROR("[ProxyService] session #{} {} failed to forward request body prefix", session_id, peer_text);
                    ctx.close();
                    upstream_ctx.close();
                    co_return;
                }
            }

            auto first_byte_probe = co_await upstream_ctx.read_async(static_cast<uint32_t>(config.upstream_first_byte_timeout_ms));
            if (first_byte_probe.status != yuan::coroutine::IoStatus::success ||
                first_byte_probe.data.readable_bytes() == 0) {
                LOG_WARN("[ProxyService] session #{} {} upstream first-byte timeout/failure status={} target={}:{}",
                         session_id,
                         peer_text,
                         static_cast<int>(first_byte_probe.status),
                         connect_target.host,
                         connect_target.port);
                if (metrics) {
                    metrics->idle_timeouts.fetch_add(1, std::memory_order_relaxed);
                }
                (void)co_await write_http_text_async(ctx, 504, "Gateway Timeout", "upstream first-byte timeout");
                upstream_ctx.close();
                ctx.close();
                co_return;
            }

            ::yuan::buffer::ByteBuffer first_chunk(first_byte_probe.data.readable_bytes());
            first_chunk.append(first_byte_probe.data.readable_span());
            const auto first_chunk_size = static_cast<uint64_t>(first_byte_probe.data.readable_bytes());
            if (!co_await write_buffer_async(ctx, first_chunk, static_cast<uint32_t>(config.idle_timeout_ms))) {
                upstream_ctx.close();
                ctx.close();
                co_return;
            }

            bytes_down = first_chunk_size;
            std::string response_probe;
            bool response_length_known = false;
            uint64_t expected_response_bytes = 0;
            auto update_expected_response_bytes = [&]() {
                if (response_length_known) {
                    return;
                }
                const auto header_end = response_probe.find("\r\n\r\n");
                if (header_end == std::string::npos) {
                    return;
                }
                const auto response_headers = parse_header_map(response_probe);
                auto len_it = response_headers.find("content-length");
                if (len_it == response_headers.end()) {
                    return;
                }
                try {
                    expected_response_bytes = static_cast<uint64_t>(header_end + 4) + std::stoull(len_it->second);
                    response_length_known = true;
                } catch (...) {
                }
            };
            {
                const auto span = first_byte_probe.data.readable_span();
                response_probe.append(span.data(), span.size());
                update_expected_response_bytes();
            }

            while (true) {
                if (response_length_known && bytes_down >= expected_response_bytes) {
                    close_reason = "upstream_closed";
                    break;
                }

                auto chunk_result = co_await upstream_ctx.read_async(static_cast<uint32_t>(config.idle_timeout_ms));
                if (chunk_result.status != yuan::coroutine::IoStatus::success) {
                    if (chunk_result.status == yuan::coroutine::IoStatus::timed_out) {
                        close_reason = "idle_timeout";
                        if (metrics) {
                            metrics->idle_timeouts.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (chunk_result.status == yuan::coroutine::IoStatus::connection_error) {
                        close_reason = "upstream_error";
                    } else {
                        close_reason = "upstream_closed";
                    }
                    break;
                }

                const auto chunk_bytes = static_cast<uint64_t>(chunk_result.data.readable_bytes());
                if (chunk_bytes == 0) {
                    close_reason = "upstream_closed";
                    break;
                }

                if (!co_await write_buffer_async(ctx, chunk_result.data, static_cast<uint32_t>(config.idle_timeout_ms))) {
                    close_reason = "client_write_failed";
                    break;
                }

                bytes_down += chunk_bytes;
                if (!response_length_known && response_probe.size() < kResponseProbeMaxBytes) {
                    const auto span = chunk_result.data.readable_span();
                    response_probe.append(span.data(), span.size());
                    update_expected_response_bytes();
                }
            }

            ctx.close();
            upstream_ctx.close();

            if (on_state_change) {
                on_state_change(ProxySessionState::closing, "relay_completed");
                on_state_change(ProxySessionState::closed, "session_completed");
            }
            if (completed_sessions) {
                completed_sessions->fetch_add(1, std::memory_order_relaxed);
            }
            if (host) {
                yuan::server::ProxySessionCompletedEvent evt;
                evt.session_id = session_id;
                evt.service_name = "proxy";
                evt.client_addr = peer_text;
                evt.method = method;
                evt.target_addr = connect_target.host + ":" + std::to_string(connect_target.port);
                evt.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at).count());
                evt.bytes_up = bytes_up;
                evt.bytes_down = bytes_down;
                evt.close_reason = close_reason;
                host->publish_custom(yuan::server::events::proxy_session_completed, std::move(evt));
            }
            increment_close_reason_counter(metrics, close_reason);
            LOG_INFO("[ProxyService] session #{} client={} method={} target={} upstream={} relay finished close_reason={} bytes_up={} bytes_down={}",
                     session_id,
                     peer_text,
                     method,
                     connect_target.host + ":" + std::to_string(connect_target.port),
                     upstream_peer_text,
                     close_reason,
                     bytes_up,
                     bytes_down);
            LOG_INFO("[ProxyService] session #{} client={} method={} target={} timing total_ms={} request_read_ms={} upstream_connect_ms={} relay_ms={}",
                     session_id,
                     peer_text,
                     method,
                     connect_target.host + ":" + std::to_string(connect_target.port),
                     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started_at).count()),
                     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         request_read_completed_at - started_at).count()),
                     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         upstream_connected_at - request_read_completed_at).count()),
                     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - upstream_connected_at).count()));

            lifecycle_completed = true;
            LOG_INFO("[ProxyService] session #{} client={} method={} target={} upstream={} connection closed",
                     session_id,
                     peer_text,
                     method,
                     connect_target.host + ":" + std::to_string(connect_target.port),
                     upstream_peer_text);
            co_return;
        }

        auto relay_state = std::make_shared<RelaySharedState>();
        relay_state->runtime = runtime;
        relay_state->client_connection = yuan::net::ConnectionHandle(ctx.connection());
        relay_state->upstream_connection = yuan::net::ConnectionHandle(connect_result.connection);
        relay_state->client_addr = peer_text;
        relay_state->upstream_addr = upstream_peer_text;
        relay_state->method = method;
        relay_state->target_host = connect_target.host;
        relay_state->target_port = connect_target.port;
        relay_state->remaining_relays.store(2, std::memory_order_relaxed);
        relay_state->relays_completed.reset(runtime.event_loop());
        relay_state->max_session_buffer_bytes = config.max_session_buffer_bytes;
        relay_state->max_total_tunnel_memory = config.max_total_tunnel_memory;
        relay_state->total_tunnel_memory = total_tunnel_memory;
        relay_state->on_half_close = [on_state_change](bool client_half_closed) {
            if (on_state_change) {
                on_state_change(client_half_closed ? ProxySessionState::half_closed_client : ProxySessionState::half_closed_upstream,
                                client_half_closed ? "client_half_close" : "upstream_half_close");
            }
        };
        if (on_relay_ready) {
            on_relay_ready(relay_state);
        }

        auto uplink_task = relay_one_way_async(relay_state, true, static_cast<uint32_t>(config.idle_timeout_ms));
        uplink_task.resume();
        uplink_task.detach();

        auto downlink_task = relay_one_way_async(relay_state, false, static_cast<uint32_t>(config.idle_timeout_ms));
        downlink_task.resume();
        downlink_task.detach();

        co_await relay_state->relays_completed.wait_for(
            runtime.timer_manager(),
            static_cast<uint32_t>(config.idle_timeout_ms * 2 + config.drain_timeout_ms + 10000));

        std::string relay_close_reason;
        {
            std::lock_guard<std::mutex> lock(relay_state->reason_mutex);
            relay_close_reason = relay_state->close_reason;
        }

        const uint64_t total_down = relay_state->bytes_down.load(std::memory_order_relaxed);
        const bool no_upstream_payload = total_down == 0;
        const bool upstream_failed_after_connect =
            relay_close_reason == "upstream_closed" ||
            relay_close_reason == "upstream_error" ||
            relay_close_reason == "upstream_write_timeout" ||
            relay_close_reason == "idle_timeout";

        if (no_upstream_payload && upstream_failed_after_connect) {
            note_relay_reason(relay_state, "upstream_no_response");
            relay_close_reason = "upstream_no_response";
        }

        const int mapped_status = proxy_http_status_for_failure(
            relay_close_reason,
            method == "CONNECT",
            relay_state->bytes_down.load(std::memory_order_relaxed));
        (void)mapped_status;

        if (!relay_state->closed.exchange(true, std::memory_order_acq_rel)) {
            if (relay_state->client_connection) {
                relay_state->client_connection->close();
            }
            if (relay_state->upstream_connection) {
                relay_state->upstream_connection->close();
            }
        }

        if (on_state_change) {
            on_state_change(ProxySessionState::closing, "relay_completed");
        }

        if (completed_sessions) {
            completed_sessions->fetch_add(1, std::memory_order_relaxed);
        }
        if (host) {
            yuan::server::ProxySessionCompletedEvent evt;
            evt.session_id = session_id;
            evt.service_name = "proxy";
            evt.client_addr = peer_text;
            evt.method = method;
            evt.target_addr = connect_target.host + ":" + std::to_string(connect_target.port);
            evt.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at).count());
            evt.bytes_up = relay_state->bytes_up.load(std::memory_order_relaxed);
            evt.bytes_down = relay_state->bytes_down.load(std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(relay_state->reason_mutex);
                evt.close_reason = relay_state->close_reason;
            }
            host->publish_custom(yuan::server::events::proxy_session_completed, std::move(evt));
        }
        {
            std::lock_guard<std::mutex> lock(relay_state->reason_mutex);
            relay_close_reason = relay_state->close_reason;
        }
        increment_close_reason_counter(metrics, relay_close_reason);
        LOG_INFO("[ProxyService] session #{} client={} method={} target={} upstream={} relay finished close_reason={} bytes_up={} bytes_down={}",
                 session_id,
                 peer_text,
                 method,
                 connect_target.host + ":" + std::to_string(connect_target.port),
                 upstream_peer_text,
                 relay_close_reason,
                 relay_state->bytes_up.load(std::memory_order_relaxed),
                 relay_state->bytes_down.load(std::memory_order_relaxed));
        LOG_INFO("[ProxyService] session #{} client={} method={} target={} timing total_ms={} request_read_ms={} upstream_connect_ms={} relay_ms={}",
                 session_id,
                 peer_text,
                 method,
                 connect_target.host + ":" + std::to_string(connect_target.port),
                 static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - started_at).count()),
                 static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     request_read_completed_at - started_at).count()),
                 static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     upstream_connected_at - request_read_completed_at).count()),
                 static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - upstream_connected_at).count()));

        if (on_state_change) {
            on_state_change(ProxySessionState::closed, "session_completed");
        }
        lifecycle_completed = true;

            LOG_INFO("[ProxyService] session #{} client={} method={} target={} upstream={} connection closed",
                     session_id,
                     peer_text,
                     method,
                     connect_target.host + ":" + std::to_string(connect_target.port),
                     upstream_peer_text);
        } catch (const std::exception &ex) {
            LOG_ERROR("[ProxyService] session #{} {} unhandled exception: {}", session_id, peer_text, ex.what());
            ctx.abort();
            co_return;
        } catch (...) {
            LOG_ERROR("[ProxyService] session #{} {} unhandled non-standard exception", session_id, peer_text);
            ctx.abort();
            co_return;
        }
    }
}

namespace yuan::server
{
    class ProxyService::ProxyServiceData
    {
    public:
        struct SessionContext
        {
            uint64_t session_id = 0;
            yuan::net::ConnectionHandle client_connection;
            yuan::net::ConnectionHandle upstream_connection;
            std::shared_ptr<RelaySharedState> relay_state;
            std::string client_addr;
            std::string client_ip;
            std::string method;
            std::string target_addr;
            std::string target_host;
            int target_port = 0;
            std::string upstream_addr;
            std::atomic<ProxySessionState> state{ ProxySessionState::accepted };
            std::atomic_bool finished{ false };
            std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point last_state_change_at = created_at;
            uint64_t last_bytes_up = 0;
            uint64_t last_bytes_down = 0;
            std::mutex mutex;
        };

        yuan::net::AsyncListenerHost listener;
        std::unique_ptr<yuan::net::NetworkRuntime> owned_runtime;
        yuan::coroutine::Task<void> accept_task;
        yuan::timer::TimerHandle session_sweep_timer;
        uint64_t session_sweep_ticks = 0;
        std::mutex mutex;
        std::vector<std::shared_ptr<SessionContext>> sessions;
    };

    ProxyService::ProxyService(ProxyServiceConfig config)
        : config_(std::move(config)),
          host_({ "proxy", "http-connect", config_.port }),
          data_(std::make_unique<ProxyServiceData>())
    {
    }

    ProxyService::~ProxyService()
    {
        stop();
    }

    bool ProxyService::init()
    {
        stop_requested_.store(false, std::memory_order_relaxed);
        active_sessions_.store(0, std::memory_order_relaxed);
        accepted_sessions_.store(0, std::memory_order_relaxed);
        rejected_sessions_.store(0, std::memory_order_relaxed);
        completed_sessions_.store(0, std::memory_order_relaxed);
        next_session_id_.store(1, std::memory_order_relaxed);
        total_tunnel_memory_.store(0, std::memory_order_relaxed);
        total_bytes_up_.store(0, std::memory_order_relaxed);
        total_bytes_down_.store(0, std::memory_order_relaxed);
        metrics_.header_timeouts.store(0, std::memory_order_relaxed);
        metrics_.connect_timeouts.store(0, std::memory_order_relaxed);
        metrics_.idle_timeouts.store(0, std::memory_order_relaxed);
        metrics_.closes_by_client.store(0, std::memory_order_relaxed);
        metrics_.closes_by_upstream.store(0, std::memory_order_relaxed);
        metrics_.closes_by_ssrf.store(0, std::memory_order_relaxed);
        metrics_.closes_by_acl.store(0, std::memory_order_relaxed);

        if (config_.port <= 0 || config_.port > 65535) {
            LOG_ERROR("[ProxyService] invalid port={}, must be 1-65535", config_.port);
            return false;
        }
        if (config_.max_active_sessions <= 0) {
            LOG_WARN("[ProxyService] max_active_sessions={} invalid, defaulting to 4096", config_.max_active_sessions);
            config_.max_active_sessions = 4096;
        }
        if (config_.header_timeout_ms <= 0) {
            LOG_WARN("[ProxyService] header_timeout_ms={} invalid, defaulting to 15000", config_.header_timeout_ms);
            config_.header_timeout_ms = 15000;
        }
        if (config_.upstream_first_byte_timeout_ms <= 0) {
            LOG_WARN("[ProxyService] upstream_first_byte_timeout_ms={} invalid, defaulting to 15000", config_.upstream_first_byte_timeout_ms);
            config_.upstream_first_byte_timeout_ms = 15000;
        }
        if (config_.idle_timeout_ms <= 0) {
            LOG_WARN("[ProxyService] idle_timeout_ms={} invalid, defaulting to 300000", config_.idle_timeout_ms);
            config_.idle_timeout_ms = 300000;
        }
        if (config_.connect_timeout_ms <= 0) {
            LOG_WARN("[ProxyService] connect_timeout_ms={} invalid, defaulting to 10000", config_.connect_timeout_ms);
            config_.connect_timeout_ms = 10000;
        }
        if (!config_.basic_auth_user.empty() && config_.basic_auth_password.empty()) {
            LOG_WARN("[ProxyService] basic_auth_user set but basic_auth_password is empty");
        }
        if (!config_.allow_targets.empty() && !config_.deny_targets.empty()) {
            LOG_WARN("[ProxyService] both allow_targets and deny_targets are set; deny is evaluated first, then allow acts as whitelist");
        }

        data_->accept_task = {};
        data_->session_sweep_timer.reset();
        data_->session_sweep_ticks = 0;
        data_->listener.close();
        data_->owned_runtime = shared_runtime_ ? nullptr : std::make_unique<yuan::net::NetworkRuntime>();

        auto *runtime = shared_runtime_ ? shared_runtime_ : data_->owned_runtime.get();
        if (!runtime) {
            return false;
        }

        return data_->listener.bind(config_.listen_host, static_cast<uint16_t>(config_.port), *runtime);
    }

    void ProxyService::start()
    {
        if (shared_runtime_) {
            host_.start_inline([this]() { serve_loop(); });
            return;
        }
        host_.start([this]() { serve_loop(); });
    }

    void ProxyService::stop()
    {
        stop_requested_.store(true, std::memory_order_relaxed);
        auto *runtime = data_->listener.runtime();
        if (runtime) {
            runtime->dispatch([this]() {
                data_->listener.close();
                data_->session_sweep_timer.cancel();
                data_->session_sweep_timer.reset();
            });
        } else {
            data_->listener.close();
            data_->session_sweep_timer.cancel();
            data_->session_sweep_timer.reset();
        }

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(std::max(config_.drain_timeout_ms, 0));
        while (std::chrono::steady_clock::now() < deadline) {
            reap_finished_sessions();
            bool all_finished = true;
            {
                std::lock_guard<std::mutex> lock(data_->mutex);
                all_finished = std::all_of(data_->sessions.begin(), data_->sessions.end(),
                                           [](const std::shared_ptr<ProxyServiceData::SessionContext> &session) {
                                               return !session || session->finished.load(std::memory_order_acquire);
                                           });
            }
            if (all_finished) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::vector<std::shared_ptr<ProxyServiceData::SessionContext>> sessions;
        {
            std::lock_guard<std::mutex> lock(data_->mutex);
            sessions = data_->sessions;
        }
        for (const auto &session : sessions) {
            if (!session || session->finished.load(std::memory_order_acquire)) {
                continue;
            }
            LOG_WARN("[ProxyService] session #{} did not drain within {} ms",
                     session->session_id,
                     std::max(config_.drain_timeout_ms, 0));
            if (runtime) {
                runtime->dispatch([session]() {
                    if (session->client_connection) {
                        session->client_connection->close();
                    }
                    if (session->upstream_connection) {
                        session->upstream_connection->close();
                    }
                });
            } else {
                if (session->client_connection) {
                    session->client_connection->close();
                }
                if (session->upstream_connection) {
                    session->upstream_connection->close();
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(data_->mutex);
            data_->sessions.clear();
        }

        host_.stop([this]() {
            if (data_->owned_runtime) {
                data_->owned_runtime->stop();
            }
        });
        data_->accept_task = {};
    }

    void ProxyService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void ProxyService::serve_loop()
    {
        LOG_INFO("[ProxyService] listening on {}:{}, max_active={}, basic_auth={}",
                 config_.listen_host,
                 config_.port,
                 config_.max_active_sessions,
                 config_.basic_auth_user.empty() ? "off" : "on");
        LOG_INFO("[ProxyService] limits: max_per_client={}, max_header_bytes={}, max_session_buffer_bytes={}, max_total_tunnel_memory={}, header_timeout_ms={}, upstream_first_byte_timeout_ms={}, connect_timeout_ms={}, idle_timeout_ms={}",
                 config_.max_sessions_per_client,
                 config_.max_header_bytes,
                 config_.max_session_buffer_bytes,
                 config_.max_total_tunnel_memory,
                 config_.header_timeout_ms,
                 config_.upstream_first_byte_timeout_ms,
                 config_.connect_timeout_ms,
                 config_.idle_timeout_ms);
        LOG_INFO("[ProxyService] traffic log: per-second aggregate up/down enabled");

        auto publish_state_transition = [this](const std::shared_ptr<ProxyServiceData::SessionContext> &session,
                                               ProxySessionState next_state,
                                               std::string_view reason) {
            const ProxySessionState previous = session->state.exchange(next_state, std::memory_order_acq_rel);
            std::string client_addr;
            std::string method;
            std::string target_addr;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->last_state_change_at = std::chrono::steady_clock::now();
                client_addr = session->client_addr;
                method = session->method;
                target_addr = session->target_addr;
            }
            if (previous == next_state) {
                return;
            }

            yuan::server::ProxySessionStateChangedEvent evt;
            evt.session_id = session->session_id;
            evt.service_name = "proxy";
            evt.client_addr = std::move(client_addr);
            evt.method = std::move(method);
            evt.target_addr = std::move(target_addr);
            evt.previous_state = proxy_session_state_name(previous);
            evt.current_state = proxy_session_state_name(next_state);
            evt.reason = std::string(reason);
            host_.publish_custom(yuan::server::events::proxy_session_state_changed, std::move(evt));

            LOG_INFO("[ProxyService] session #{} state {} -> {} reason={}",
                     session->session_id,
                     proxy_session_state_name(previous),
                     proxy_session_state_name(next_state),
                     reason);
        };

        auto *runtime = data_->listener.runtime();
        if (runtime) {
            data_->session_sweep_timer = runtime->schedule_periodic(
                1000,
                1000,
                [this, publish_state_transition]() {
                    std::vector<std::shared_ptr<ProxyServiceData::SessionContext>> sessions;
                    {
                        std::lock_guard<std::mutex> lock(data_->mutex);
                        sessions = data_->sessions;
                    }

                    std::size_t accepted_count = 0;
                    std::size_t reading_count = 0;
                    std::size_t connecting_count = 0;
                    std::size_t established_count = 0;
                    std::size_t half_closed_client_count = 0;
                    std::size_t half_closed_upstream_count = 0;
                    std::size_t closing_count = 0;

                    uint64_t sweep_bytes_up = 0;
                    uint64_t sweep_bytes_down = 0;

                    const auto now = std::chrono::steady_clock::now();
                    for (const auto &session : sessions) {
                        if (!session || session->finished.load(std::memory_order_acquire)) {
                            continue;
                        }

                        const auto state = session->state.load(std::memory_order_acquire);
                        switch (state) {
                        case ProxySessionState::accepted:
                            ++accepted_count;
                            break;
                        case ProxySessionState::reading_request:
                            ++reading_count;
                            break;
                        case ProxySessionState::connecting_upstream:
                            ++connecting_count;
                            break;
                        case ProxySessionState::established:
                            ++established_count;
                            break;
                        case ProxySessionState::half_closed_client:
                            ++half_closed_client_count;
                            break;
                        case ProxySessionState::half_closed_upstream:
                            ++half_closed_upstream_count;
                            break;
                        case ProxySessionState::closing:
                            ++closing_count;
                            break;
                        case ProxySessionState::closed:
                            break;
                        }

                        std::chrono::steady_clock::time_point last_change;
                        std::string client_addr;
                        std::string method;
                        std::string target_addr;
                        std::string upstream_addr;
                        std::shared_ptr<RelaySharedState> relay_state;
                        uint64_t prev_bytes_up = 0;
                        uint64_t prev_bytes_down = 0;
                        {
                            std::lock_guard<std::mutex> lock(session->mutex);
                            last_change = session->last_state_change_at;
                            client_addr = session->client_addr;
                            method = session->method;
                            target_addr = session->target_addr;
                            upstream_addr = session->upstream_addr;
                            relay_state = session->relay_state;
                            prev_bytes_up = session->last_bytes_up;
                            prev_bytes_down = session->last_bytes_down;
                        }
                        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change).count();
                        const bool request_phase_timed_out =
                            (state == ProxySessionState::accepted || state == ProxySessionState::reading_request) &&
                            elapsed_ms > config_.header_timeout_ms;
                        const bool connect_phase_timed_out =
                            state == ProxySessionState::connecting_upstream &&
                            elapsed_ms > config_.connect_timeout_ms;

                        if (!request_phase_timed_out && !connect_phase_timed_out) {
                            if ((state == ProxySessionState::established ||
                                 state == ProxySessionState::half_closed_client ||
                                 state == ProxySessionState::half_closed_upstream) && relay_state) {
                                const uint64_t total_up = relay_state->bytes_up.load(std::memory_order_relaxed);
                                const uint64_t total_down = relay_state->bytes_down.load(std::memory_order_relaxed);
                                const uint64_t delta_up = total_up >= prev_bytes_up ? (total_up - prev_bytes_up) : total_up;
                                const uint64_t delta_down = total_down >= prev_bytes_down ? (total_down - prev_bytes_down) : total_down;
                                sweep_bytes_up += delta_up;
                                sweep_bytes_down += delta_down;
                                LOG_INFO("[ProxyService] session #{} traffic 1s client={} method={} target={} upstream={} up_Bps={} down_Bps={} total_up={} total_down={}",
                                         session->session_id,
                                         client_addr,
                                         method.empty() ? "-" : method,
                                         target_addr.empty() ? "-" : target_addr,
                                         upstream_addr.empty() ? "-" : upstream_addr,
                                         delta_up,
                                         delta_down,
                                         total_up,
                                         total_down);
                                std::lock_guard<std::mutex> lock(session->mutex);
                                session->last_bytes_up = total_up;
                                session->last_bytes_down = total_down;
                            }
                            continue;
                        }

                        publish_state_transition(session,
                                                 ProxySessionState::closing,
                                                 request_phase_timed_out ? "header_phase_timeout" : "connect_phase_timeout");
                        if (session->client_connection) {
                            session->client_connection->close();
                        }
                        if (session->upstream_connection) {
                            session->upstream_connection->close();
                        }
                    }

                    ++data_->session_sweep_ticks;
                    const uint64_t aggregate_up = total_bytes_up_.fetch_add(sweep_bytes_up, std::memory_order_relaxed) + sweep_bytes_up;
                    const uint64_t aggregate_down = total_bytes_down_.fetch_add(sweep_bytes_down, std::memory_order_relaxed) + sweep_bytes_down;
                    const uint64_t process_working_set = current_process_working_set_bytes();
                    LOG_INFO("[ProxyService] traffic aggregate 1s active={} up_Bps={} down_Bps={} total_up={} total_down={} tunnel_mem={}B process_mem={}B",
                             active_sessions_.load(std::memory_order_relaxed),
                             sweep_bytes_up,
                             sweep_bytes_down,
                             aggregate_up,
                             aggregate_down,
                             total_tunnel_memory_.load(std::memory_order_relaxed),
                             process_working_set);
                    const int snapshot_interval_ms = std::max(config_.session_snapshot_interval_ms, 0);
                    const bool should_publish_snapshot =
                        snapshot_interval_ms > 0 &&
                        ((data_->session_sweep_ticks * 1000ULL) % static_cast<uint64_t>(snapshot_interval_ms) == 0);
                    if (should_publish_snapshot) {
                        yuan::server::ProxySessionSnapshotEvent evt;
                        evt.service_name = "proxy";
                        evt.accepted_sessions = static_cast<uint32_t>(accepted_count);
                        evt.reading_request_sessions = static_cast<uint32_t>(reading_count);
                        evt.connecting_upstream_sessions = static_cast<uint32_t>(connecting_count);
                        evt.established_sessions = static_cast<uint32_t>(established_count);
                        evt.half_closed_client_sessions = static_cast<uint32_t>(half_closed_client_count);
                        evt.half_closed_upstream_sessions = static_cast<uint32_t>(half_closed_upstream_count);
                        evt.closing_sessions = static_cast<uint32_t>(closing_count);
                        evt.active_sessions = static_cast<uint32_t>(active_sessions_.load(std::memory_order_relaxed));
                        evt.total_accepted = accepted_sessions_.load(std::memory_order_relaxed);
                        evt.total_rejected = rejected_sessions_.load(std::memory_order_relaxed);
                        evt.total_completed = completed_sessions_.load(std::memory_order_relaxed);
                        evt.header_timeouts = metrics_.header_timeouts.load(std::memory_order_relaxed);
                        evt.connect_timeouts = metrics_.connect_timeouts.load(std::memory_order_relaxed);
                        evt.idle_timeouts = metrics_.idle_timeouts.load(std::memory_order_relaxed);
                        evt.closes_by_client = metrics_.closes_by_client.load(std::memory_order_relaxed);
                        evt.closes_by_upstream = metrics_.closes_by_upstream.load(std::memory_order_relaxed);
                        evt.closes_by_ssrf = metrics_.closes_by_ssrf.load(std::memory_order_relaxed);
                        evt.closes_by_acl = metrics_.closes_by_acl.load(std::memory_order_relaxed);
                        host_.publish_custom(yuan::server::events::proxy_session_snapshot, evt);

                        LOG_INFO("[ProxyService] session snapshot accepted={}, reading_request={}, connecting_upstream={}, established={}, half_closed_client={}, half_closed_upstream={}, closing={}, active={}, header_timeouts={}, connect_timeouts={}, idle_timeouts={}, closes_by_client={}, closes_by_upstream={}, closes_by_ssrf={}, closes_by_acl={}",
                                 accepted_count,
                                 reading_count,
                                 connecting_count,
                                 established_count,
                                 half_closed_client_count,
                                 half_closed_upstream_count,
                                 closing_count,
                                 active_sessions_.load(std::memory_order_relaxed),
                                 evt.header_timeouts,
                                 evt.connect_timeouts,
                                 evt.idle_timeouts,
                                 evt.closes_by_client,
                                 evt.closes_by_upstream,
                                 evt.closes_by_ssrf,
                                 evt.closes_by_acl);
                    }

                    reap_finished_sessions();
                });
        }

        auto accept_loop = [this, publish_state_transition]() -> yuan::coroutine::Task<void> {
            auto *listener_runtime = data_->listener.runtime();
            auto *acceptor = data_->listener.acceptor();
            if (!listener_runtime || !acceptor) {
                co_return;
            }

            auto rv = listener_runtime->runtime_view();
            while (!stop_requested_.load(std::memory_order_relaxed)) {
                reap_finished_sessions();
                auto conn = co_await yuan::coroutine::async_accept(rv, acceptor);
                if (!conn) {
                    break;
                }
                tune_proxy_stream_connection(conn);

                const uint64_t session_id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
                const std::string client_addr = format_remote_address(conn->get_remote_address());
                const std::string client_ip = format_client_identity(conn->get_remote_address());
                if (active_sessions_.load(std::memory_order_relaxed) >= config_.max_active_sessions) {
                    LOG_WARN("[ProxyService] session #{} reject client due to active session limit", session_id);
                    rejected_sessions_.fetch_add(1, std::memory_order_relaxed);

                    yuan::server::ProxySessionRejectedEvent evt;
                    evt.session_id = session_id;
                    evt.service_name = "proxy";
                    evt.client_addr = client_addr;
                    evt.reason = "max_active_limit";
                    host_.publish_custom(yuan::server::events::proxy_session_rejected, std::move(evt));

                    yuan::net::AsyncConnectionContext reject_ctx(conn, static_cast<yuan::coroutine::RuntimeView>(rv));
                    (void)co_await write_http_text_async(reject_ctx, 503, "Service Unavailable", "proxy busy");
                    reject_ctx.close();
                    continue;
                }

                if (config_.max_sessions_per_client > 0) {
                    int client_sessions = 0;
                    {
                        std::lock_guard<std::mutex> lock(data_->mutex);
                        for (const auto &session : data_->sessions) {
                            if (!session || session->finished.load(std::memory_order_acquire)) {
                                continue;
                            }
                            if (session->client_ip == client_ip) {
                                ++client_sessions;
                            }
                        }
                    }

                    if (client_sessions >= config_.max_sessions_per_client) {
                        LOG_WARN("[ProxyService] session #{} reject client {} due to per-client session limit {}",
                                 session_id,
                                 client_ip,
                                 config_.max_sessions_per_client);
                        rejected_sessions_.fetch_add(1, std::memory_order_relaxed);

                        yuan::server::ProxySessionRejectedEvent evt;
                        evt.session_id = session_id;
                        evt.service_name = "proxy";
                        evt.client_addr = client_addr;
                        evt.reason = "max_sessions_per_client";
                        host_.publish_custom(yuan::server::events::proxy_session_rejected, std::move(evt));

                        yuan::net::AsyncConnectionContext reject_ctx(conn, static_cast<yuan::coroutine::RuntimeView>(rv));
                        (void)co_await write_http_text_async(
                            reject_ctx,
                            429,
                            "Too Many Requests",
                            "too many active sessions for client");
                        reject_ctx.close();
                        continue;
                    }
                }

                accepted_sessions_.fetch_add(1, std::memory_order_relaxed);
                auto session = std::make_shared<ProxyServiceData::SessionContext>();
                session->session_id = session_id;
                session->client_connection = yuan::net::ConnectionHandle(conn);
                session->client_addr = client_addr;
                session->client_ip = client_ip;
                {
                    std::lock_guard<std::mutex> lock(data_->mutex);
                    data_->sessions.push_back(session);
                }

                yuan::net::AsyncConnectionContext ctx(conn, static_cast<yuan::coroutine::RuntimeView>(rv));
                auto task = handle_http_proxy_client_async(
                    session_id,
                    std::move(ctx),
                    static_cast<yuan::coroutine::RuntimeView>(rv),
                    config_,
                    active_sessions_,
                    &host_,
                    &completed_sessions_,
                    &metrics_,
                    &auth_rate_limiter_,
                    [session](std::string_view client_addr,
                              std::string_view method,
                              std::string_view target_addr,
                              std::string_view target_host,
                              int target_port) {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        session->client_addr = std::string(client_addr);
                        session->method = std::string(method);
                        session->target_addr = std::string(target_addr);
                        session->target_host = std::string(target_host);
                        session->target_port = target_port;
                    },
                    [publish_state_transition, session](ProxySessionState next_state, std::string_view reason) {
                        publish_state_transition(session, next_state, reason);
                    },
                    [session](const std::shared_ptr<yuan::net::Connection> &upstream) {
                        session->upstream_connection = yuan::net::ConnectionHandle(upstream);
                        if (upstream) {
                            std::lock_guard<std::mutex> lock(session->mutex);
                            session->upstream_addr = format_remote_address(upstream->get_remote_address());
                        }
                    },
                    [session](const std::shared_ptr<RelaySharedState> &relay_state) {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        session->relay_state = relay_state;
                        session->last_bytes_up = relay_state ? relay_state->bytes_up.load(std::memory_order_relaxed) : 0;
                        session->last_bytes_down = relay_state ? relay_state->bytes_down.load(std::memory_order_relaxed) : 0;
                    },
                    &total_tunnel_memory_,
                    [session]() {
                        session->finished.store(true, std::memory_order_release);
                    });
                task.resume();
                task.detach();
            }
            co_return;
        };

        data_->accept_task = accept_loop();
        data_->accept_task.resume();

        if (data_->owned_runtime) {
            data_->owned_runtime->run();
        }

        reap_finished_sessions();

        LOG_INFO("[ProxyService] listener stopped, accepted={}, rejected={}, completed={}, active={}",
                 accepted_sessions_.load(std::memory_order_relaxed),
                 rejected_sessions_.load(std::memory_order_relaxed),
                 completed_sessions_.load(std::memory_order_relaxed),
                 active_sessions_.load(std::memory_order_relaxed));
    }

    void ProxyService::reap_finished_sessions()
    {
        std::lock_guard<std::mutex> lock(data_->mutex);
        auto it = data_->sessions.begin();
        while (it != data_->sessions.end()) {
            if (*it && (*it)->finished.load(std::memory_order_acquire)) {
                it = data_->sessions.erase(it);
            } else {
                ++it;
            }
        }
    }
}
