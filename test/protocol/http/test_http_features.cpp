#include "bootstrap.h"
#include "eventbus/event_bus.h"
#include "http/http_service.h"
#include "http2/hpack_decoder.h"
#include "http2/hpack_encoder.h"
#include "http_server.h"
#include "net/runtime/network_runtime.h"
#include "platform/native_platform.h"
#include "request.h"
#include "response.h"
#include "runtime_context.h"
#include "server_service_events.h"

#include <any>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"
#include "ops/config_manager.h"
#include "ops/option.h"

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace
{
    int g_failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++g_failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }

    void close_socket(socket_t s)
    {
        if (s == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    void abortive_close_socket(socket_t s)
    {
        if (s == kInvalidSocket) {
            return;
        }

        linger opt{};
        opt.l_onoff = 1;
        opt.l_linger = 0;
#ifdef _WIN32
        (void)::setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
        (void)::setsockopt(s, SOL_SOCKET, SO_LINGER, &opt, sizeof(opt));
#endif
        close_socket(s);
    }

    bool send_all(socket_t s, const std::string &data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            const int rc = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t rc = ::send(s, data.data() + sent, data.size() - sent, 0);
#endif
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    std::size_t parse_content_length(const std::string &headers)
    {
        std::string lower = headers;
        for (char &ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        const std::string key = "content-length:";
        const auto pos = lower.find(key);
        if (pos == std::string::npos) {
            return static_cast<std::size_t>(-1);
        }

        std::size_t p = pos + key.size();
        while (p < lower.size() && std::isspace(static_cast<unsigned char>(lower[p]))) {
            ++p;
        }
        std::size_t end = p;
        while (end < lower.size() && std::isdigit(static_cast<unsigned char>(lower[end]))) {
            ++end;
        }
        if (end == p) {
            return static_cast<std::size_t>(-1);
        }
        return static_cast<std::size_t>(std::strtoull(lower.substr(p, end - p).c_str(), nullptr, 10));
    }

    bool contains_header_fragment(std::string text, const std::string &fragment)
    {
        for (char &ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text.find(fragment) != std::string::npos;
    }

    std::string trim_http_value(std::string text)
    {
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || std::isspace(static_cast<unsigned char>(text.back())))) {
            text.pop_back();
        }
        std::size_t p = 0;
        while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) {
            ++p;
        }
        if (p > 0) {
            text.erase(0, p);
        }
        return text;
    }

    std::string recv_all(socket_t s)
    {
        std::string out;
        out.reserve(8192);
        char buf[4096];
        std::size_t expect_total = static_cast<std::size_t>(-1);
        while (true) {
#ifdef _WIN32
            const int rc = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t rc = ::recv(s, buf, sizeof(buf), 0);
#endif
            if (rc <= 0) {
                break;
            }
            out.append(buf, static_cast<std::size_t>(rc));

            const auto header_end = out.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                if (expect_total == static_cast<std::size_t>(-1)) {
                    const std::size_t content_length = parse_content_length(out.substr(0, header_end + 4));
                    if (content_length != static_cast<std::size_t>(-1)) {
                        expect_total = header_end + 4 + content_length;
                    }
                }
                if (expect_total != static_cast<std::size_t>(-1) && out.size() >= expect_total) {
                    break;
                }
            }
        }
        return out;
    }

    bool recv_until_http_headers(socket_t s, std::string &out, std::size_t max_bytes = 128 * 1024)
    {
        char buf[4096];
        while (out.find("\r\n\r\n") == std::string::npos && out.size() < max_bytes) {
#ifdef _WIN32
            const int rc = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t rc = ::recv(s, buf, sizeof(buf), 0);
#endif
            if (rc <= 0) {
                return false;
            }
            out.append(buf, static_cast<std::size_t>(rc));
        }
        return out.find("\r\n\r\n") != std::string::npos;
    }

    bool wait_http_connections(yuan::server::HttpService &service, int expected, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (service.server().snapshot_server_stats().active_http_connections == expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return service.server().snapshot_server_stats().active_http_connections == expected;
    }

    socket_t connect_loopback(uint16_t port, int recv_buffer_bytes = 0)
    {
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kInvalidSocket) {
            return kInvalidSocket;
        }

#ifdef _WIN32
        const DWORD recv_timeout_ms = 2500;
        const DWORD send_timeout_ms = 2500;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&recv_timeout_ms), sizeof(recv_timeout_ms));
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&send_timeout_ms), sizeof(send_timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 500000;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        if (recv_buffer_bytes > 0) {
#ifdef _WIN32
            (void)::setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                               reinterpret_cast<const char *>(&recv_buffer_bytes),
                               sizeof(recv_buffer_bytes));
#else
            (void)::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &recv_buffer_bytes, sizeof(recv_buffer_bytes));
#endif
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(s, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(s);
            return kInvalidSocket;
        }
        return s;
    }

    uint16_t reserve_tcp_port()
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return 0;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            return 0;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(listener);
            return 0;
        }
        const uint16_t port = ntohs(bound.sin_port);
        close_socket(listener);
        return port;
    }

    std::string http_get(uint16_t port, const std::string &path, const std::string &extra_headers = {})
    {
        socket_t s = connect_loopback(port);
        if (s == kInvalidSocket) {
            return {};
        }
#ifdef _WIN32
        const DWORD recv_timeout_ms = 2500;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&recv_timeout_ms), sizeof(recv_timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 500000;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        const std::string req =
            "GET " + path + " HTTP/1.1\r\n"
                            "Host: 127.0.0.1:" +
            std::to_string(port) + "\r\n"
                                   "Connection: close\r\n" +
            extra_headers +
            "\r\n";
        if (!send_all(s, req)) {
            close_socket(s);
            return {};
        }
        std::string resp = recv_all(s);
        close_socket(s);
        return resp;
    }

    std::string raw_http_request(uint16_t port, const std::string &raw)
    {
        socket_t s = connect_loopback(port);
        if (s == kInvalidSocket) {
            return {};
        }
#ifdef _WIN32
        const DWORD recv_timeout_ms = 2500;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&recv_timeout_ms), sizeof(recv_timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 500000;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        if (!send_all(s, raw)) {
            close_socket(s);
            return {};
        }
        std::string resp = recv_all(s);
        close_socket(s);
        return resp;
    }

    std::string http_request(uint16_t port,
                             const std::string &method,
                             const std::string &path,
                             const std::string &extra_headers = {},
                             const std::string &body = {})
    {
        socket_t s = connect_loopback(port);
        if (s == kInvalidSocket) {
            return {};
        }
#ifdef _WIN32
        const DWORD recv_timeout_ms = 2500;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&recv_timeout_ms), sizeof(recv_timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 500000;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        std::string req =
            method + " " + path + " HTTP/1.1\r\n"
                                  "Host: 127.0.0.1:" +
            std::to_string(port) + "\r\n"
                                   "Connection: close\r\n";
        if (!body.empty()) {
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += extra_headers;
        req += "\r\n";
        req += body;

        if (!send_all(s, req)) {
            close_socket(s);
            return {};
        }
        std::string resp = recv_all(s);
        close_socket(s);
        return resp;
    }

    std::string http_raw_exchange(uint16_t port, const std::vector<std::string> &chunks, int delay_ms = 0)
    {
        socket_t s = connect_loopback(port);
        if (s == kInvalidSocket) {
            return {};
        }

        for (const auto &chunk : chunks) {
            if (!send_all(s, chunk)) {
                close_socket(s);
                return {};
            }
            if (delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }

        std::string resp = recv_all(s);
        close_socket(s);
        return resp;
    }

    bool recv_exact(socket_t s, char *dst, std::size_t n)
    {
        std::size_t off = 0;
        while (off < n) {
#ifdef _WIN32
            const int rc = ::recv(s, dst + off, static_cast<int>(n - off), 0);
#else
            const ssize_t rc = ::recv(s, dst + off, n - off, 0);
#endif
            if (rc <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(rc);
        }
        return true;
    }

    bool recv_http2_frame(socket_t s, std::string &header_out, std::string &payload_out)
    {
        header_out.resize(9);
        if (!recv_exact(s, header_out.data(), header_out.size())) {
            return false;
        }

        const std::size_t len =
            (static_cast<std::size_t>(static_cast<unsigned char>(header_out[0])) << 16) |
            (static_cast<std::size_t>(static_cast<unsigned char>(header_out[1])) << 8) |
            static_cast<std::size_t>(static_cast<unsigned char>(header_out[2]));

        payload_out.clear();
        if (len == 0) {
            return true;
        }
        payload_out.resize(len);
        return recv_exact(s, payload_out.data(), payload_out.size());
    }

    bool recv_http2_frames(socket_t s, std::size_t max_frames, std::vector<std::pair<std::string, std::string>> &out)
    {
        out.clear();
        out.reserve(max_frames);
        for (std::size_t i = 0; i < max_frames; ++i) {
            std::string h;
            std::string p;
            if (!recv_http2_frame(s, h, p)) {
                break;
            }
            out.emplace_back(std::move(h), std::move(p));
        }
        return !out.empty();
    }

    bool skip_server_settings(socket_t s)
    {
        std::string hdr;
        std::string payload;
        if (!recv_http2_frame(s, hdr, payload)) {
            return false;
        }
        const unsigned char type = static_cast<unsigned char>(hdr[3]);
        const unsigned char flags = static_cast<unsigned char>(hdr[4]);
        if (type == 0x04 && (flags & 0x01) == 0) {
            return true;
        }
        return false;
    }

    void hpack_append_indexed(std::string &out, std::uint32_t index)
    {
        if (index == 0 || index >= 127) {
            return;
        }
        out.push_back(static_cast<char>(0x80u | static_cast<unsigned char>(index)));
    }

    void hpack_append_literal_without_indexing(std::string &out, std::uint32_t name_index, const std::string &value)
    {
        if (name_index >= 15) {
            return;
        }
        out.push_back(static_cast<char>(name_index & 0x0f));
        if (value.size() >= 127) {
            return;
        }
        out.push_back(static_cast<char>(value.size() & 0x7f));
        out.append(value);
    }

    std::string hpack_headers(std::initializer_list<std::pair<std::string_view, std::string_view>> headers)
    {
        yuan::net::http::http2::HpackEncoder encoder;
        std::vector<std::uint8_t> block;
        for (const auto &[name, value] : headers) {
            encoder.encode_header(block, name, value);
        }
        return std::string(reinterpret_cast<const char *>(block.data()), block.size());
    }

    std::string h2_frame(std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id, const std::string &payload)
    {
        std::string frame;
        frame.resize(9 + payload.size());
        frame[0] = static_cast<char>((payload.size() >> 16) & 0xff);
        frame[1] = static_cast<char>((payload.size() >> 8) & 0xff);
        frame[2] = static_cast<char>(payload.size() & 0xff);
        frame[3] = static_cast<char>(type);
        frame[4] = static_cast<char>(flags);
        frame[5] = static_cast<char>((stream_id >> 24) & 0x7f);
        frame[6] = static_cast<char>((stream_id >> 16) & 0xff);
        frame[7] = static_cast<char>((stream_id >> 8) & 0xff);
        frame[8] = static_cast<char>(stream_id & 0xff);
        if (!payload.empty()) {
            std::memcpy(frame.data() + 9, payload.data(), payload.size());
        }
        return frame;
    }

    std::string h2_window_update(std::uint32_t stream_id, std::uint32_t increment)
    {
        std::string payload;
        payload.resize(4);
        payload[0] = static_cast<char>((increment >> 24) & 0x7f);
        payload[1] = static_cast<char>((increment >> 16) & 0xff);
        payload[2] = static_cast<char>((increment >> 8) & 0xff);
        payload[3] = static_cast<char>(increment & 0xff);
        return h2_frame(0x08, 0x00, stream_id, payload);
    }

    std::optional<std::string> h2_header_value(const std::string &payload, std::string_view name)
    {
        yuan::net::http::http2::HpackDecoder decoder;
        std::vector<std::uint8_t> block(payload.begin(), payload.end());
        std::vector<yuan::net::http::http2::HpackHeaderField> fields;
        if (!decoder.decode(block, fields)) {
            return std::nullopt;
        }
        for (const auto &field : fields) {
            if (field.name == name) {
                return field.value;
            }
        }
        return std::nullopt;
    }

    void dump_http2_frames(std::string_view label, const std::vector<std::pair<std::string, std::string>> &frames)
    {
        std::cerr << "[DEBUG] " << label << " received " << frames.size() << " frame(s)\n";
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto &h = frames[i].first;
            if (h.size() < 9) {
                std::cerr << "[DEBUG]   #" << i << " short header len=" << h.size() << "\n";
                continue;
            }
            const std::size_t len =
                (static_cast<std::size_t>(static_cast<unsigned char>(h[0])) << 16) |
                (static_cast<std::size_t>(static_cast<unsigned char>(h[1])) << 8) |
                static_cast<std::size_t>(static_cast<unsigned char>(h[2]));
            const auto type = static_cast<unsigned char>(h[3]);
            const auto flags = static_cast<unsigned char>(h[4]);
            const std::uint32_t stream_id =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(h[5]) & 0x7f) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(h[6])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(h[7])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(h[8]));
            std::cerr << "[DEBUG]   #" << i
                      << " type=" << static_cast<int>(type)
                      << " flags=0x" << std::hex << static_cast<int>(flags) << std::dec
                      << " stream=" << stream_id
                      << " len=" << len
                      << " payload=" << frames[i].second.size()
                      << "\n";
        }
    }

    std::optional<std::string> hpack_field_value(const std::vector<yuan::net::http::http2::HpackHeaderField> &fields,
                                                 std::string_view name)
    {
        for (const auto &field : fields) {
            if (field.name == name) {
                return field.value;
            }
        }
        return std::nullopt;
    }

    void test_hpack_decoder_regressions()
    {
        using yuan::net::http::http2::HpackDecoder;
        using yuan::net::http::http2::HpackEncoder;
        using yuan::net::http::http2::HpackHeaderField;

        const std::vector<std::uint8_t> python_hpack_sample = {
            0x82,
            0x44,
            0x86,
            0x60,
            0x75,
            0x99,
            0x84,
            0x95,
            0x09,
            0x87,
            0x41,
            0x8a,
            0xa0,
            0xe4,
            0x1d,
            0x13,
            0x9d,
            0x09,
            0xb8,
            0xf0,
            0x1e,
            0x07,
        };

        HpackDecoder decoder;
        std::vector<HpackHeaderField> fields;
        check(decoder.decode(python_hpack_sample, fields), "HPACK decoder should decode known Huffman sample");
        check(fields.size() == 4, "HPACK decoder sample should contain 4 fields");
        check(hpack_field_value(fields, ":method") == std::optional<std::string>("GET"),
              "HPACK decoder sample should decode :method");
        check(hpack_field_value(fields, ":path") == std::optional<std::string>("/api/test"),
              "HPACK decoder sample should decode :path");
        check(hpack_field_value(fields, ":scheme") == std::optional<std::string>("https"),
              "HPACK decoder sample should decode :scheme");
        check(hpack_field_value(fields, ":authority") == std::optional<std::string>("localhost:8080"),
              "HPACK decoder sample should decode :authority");

        HpackEncoder encoder;
        std::vector<std::uint8_t> roundtrip;
        encoder.encode_header(roundtrip, ":method", "POST");
        encoder.encode_header(roundtrip, ":path", "/__h2_echo");
        encoder.encode_header(roundtrip, ":authority", "local.test");
        encoder.encode_header(roundtrip, "x-repeat", "alpha-value");
        encoder.encode_header(roundtrip, "x-repeat", "alpha-value");

        HpackDecoder roundtrip_decoder;
        fields.clear();
        check(roundtrip_decoder.decode(roundtrip, fields), "HPACK decoder should decode encoder roundtrip");
        check(fields.size() == 5, "HPACK encoder roundtrip should contain 5 fields");
        check(hpack_field_value(fields, ":method") == std::optional<std::string>("POST"),
              "HPACK encoder roundtrip should decode :method");
        check(hpack_field_value(fields, ":path") == std::optional<std::string>("/__h2_echo"),
              "HPACK encoder roundtrip should decode :path");
        check(hpack_field_value(fields, ":authority") == std::optional<std::string>("local.test"),
              "HPACK encoder roundtrip should decode :authority");
        check(fields[3].name == "x-repeat" && fields[3].value == "alpha-value" &&
                  fields[4].name == "x-repeat" && fields[4].value == "alpha-value",
              "HPACK decoder should resolve dynamic table indexed references");
    }

    void test_http_caps_and_proxy_stats(uint16_t port)
    {
        const std::string caps = http_get(port, "/__http_caps");
        check(caps.find("200") != std::string::npos, "__http_caps should return 200");
        check(caps.find("\"http1\":true") != std::string::npos, "__http_caps should report http1");
        check(caps.find("\"http2\":false") != std::string::npos, "__http_caps should default http2=false");
        check(caps.find("\"http3\":false") != std::string::npos, "__http_caps should default http3=false");

        const std::string stats = http_get(port, "/__proxy_stats");
        check(stats.find("200") != std::string::npos, "__proxy_stats should return 200");

        const std::string mini_stats = http_get(port, "/__mini_nginx_stats");
        check(mini_stats.find("200") != std::string::npos, "__mini_nginx_stats should return 200");
        check(mini_stats.find("\"server\"") != std::string::npos, "__mini_nginx_stats should include server section");
        check(mini_stats.find("\"proxy\"") != std::string::npos, "__mini_nginx_stats should include proxy section");
    }

    void test_http1_split_header_and_invalid_content_length(uint16_t port)
    {
        const std::string split_resp = http_raw_exchange(port, {"GET /__http_caps HTTP/1.1\r\nHost: 127.0.0.1\r\n", "Connection: close\r\n\r\n"}, 10);
        check(split_resp.find("200") != std::string::npos, "split HTTP/1.1 header should parse after more bytes");

        const std::string bad_len_resp = http_raw_exchange(port, {"POST /__echo_body HTTP/1.1\r\n"
                                                                  "Host: 127.0.0.1\r\n"
                                                                  "Content-Type: text/plain\r\n"
                                                                  "Content-Length: nope\r\n"
                                                                  "Connection: close\r\n\r\n"});
        check(bad_len_resp.find("400") != std::string::npos, "invalid Content-Length should return 400");
    }

    void test_http1_chunked_request(uint16_t port)
    {
        const std::string resp = http_raw_exchange(port, {"POST /__echo_body HTTP/1.1\r\n"
                                                          "Host: 127.0.0.1\r\n"
                                                          "Content-Type: text/plain\r\n"
                                                          "Transfer-Encoding: Chunked\r\n"
                                                          "Connection: close\r\n\r\n",
                                                          "4\r\nWiki\r\n", "5\r\npedia\r\n", "0\r\n\r\n"},
                                                   10);

        check(resp.find("200") != std::string::npos, "chunked HTTP/1.1 request should return 200");
        check(resp.find("Wikipedia") != std::string::npos, "chunked HTTP/1.1 body should be reassembled");
    }

    void test_proxy_unmatched_route_returns_404(uint16_t port)
    {
        const std::string resp = http_get(port, "/api/not-found");
        check(!resp.empty(), "proxy unmatched route response should not be empty");
        check(resp.find("404") != std::string::npos,
              "unmatched proxy route should return 404");
    }

    void test_proxy_connect_failure_returns_502(uint16_t port)
    {
        const auto begin = std::chrono::steady_clock::now();
        const std::string resp = http_get(port, "/proxy-fail/connect");
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count();

        check(!resp.empty(), "proxy connect-failure response should not be empty");
        check(resp.find("502") != std::string::npos || resp.find("504") != std::string::npos,
              "proxy connect failure should return a gateway error");
        check(elapsed_ms < 3000,
              "proxy connect failure should return quickly");
    }

    void test_proxy_strip_prefix_rewrite(uint16_t port)
    {
        const std::string resp = http_get(port, "/proxy-rewrite/demo");
        check(resp.find("200") != std::string::npos,
              "proxy rewrite route should return 200");
        check(resp.find("GET /rewritten/demo HTTP/1.1") != std::string::npos,
              "proxy rewrite route should forward rewritten path");
    }

    void test_proxy_strip_prefix_empty_path(uint16_t port)
    {
        const std::string resp = http_get(port, "/proxy-root/");
        check(resp.find("200") != std::string::npos,
              "proxy root strip route should return 200");
        check(resp.find("GET / HTTP/1.1") != std::string::npos,
              "proxy root strip should normalize forwarded path to /");
    }

    void test_proxy_header_controls(uint16_t port, const std::filesystem::path &upstream_log_path)
    {
        {
            std::ofstream clear(upstream_log_path, std::ios::binary | std::ios::trunc);
        }

        const std::string resp = http_get(
            port,
            "/proxy-headers/demo?x=1",
            "Host: client.local\r\n"
            "X-Remove-Me: secret\r\n");
        check(resp.find("200") != std::string::npos,
              "proxy header-control route should return 200");

        std::ifstream in(upstream_log_path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string upstream_req = buffer.str();
        check(upstream_req.find("Host: client.local") != std::string::npos,
              "proxy preserve_host should forward original host");
        check(upstream_req.find("X-Forwarded-Host: client.local") != std::string::npos,
              "proxy_set_header should expand $host");
        check(upstream_req.find("X-Request-Uri: /proxy-headers/demo?x=1") != std::string::npos,
              "proxy_set_header should expand $request_uri");
        check(upstream_req.find("X-Proxy-Added:") != std::string::npos,
              "proxy_set_header should add custom header");
        check(upstream_req.find("X-Remove-Me:") == std::string::npos,
              "hide_request_headers should remove configured request headers");
        check(resp.find("X-Upstream-Secret:") == std::string::npos,
              "proxy_hide_header should remove configured upstream response headers");
        check(resp.find("X-Replace-Me: edge") != std::string::npos,
              "proxy_set_response_header should replace upstream response headers");
        check(resp.find("X-Proxy-Response: yes") != std::string::npos,
              "proxy_set_response_header should add configured response headers");
    }

    void test_proxy_redirect(uint16_t port)
    {
        const std::string resp = http_get(port, "/proxy-redirect/demo");
        check(resp.find("200") != std::string::npos,
              "proxy_redirect route should return 200");
        check(resp.find("Location: /public/login") != std::string::npos,
              "proxy_redirect should rewrite upstream Location header");
        check(resp.find("Location: http://upstream.local/internal/login") == std::string::npos,
              "proxy_redirect should remove original upstream Location value");
    }

    void test_proxy_method_limit(uint16_t port)
    {
        const std::string resp = http_request(port, "POST", "/proxy-headers/demo", {}, "blocked");
        check(resp.find("405") != std::string::npos,
              "proxy location method limit should return 405");
        check(resp.find("Allow: GET") != std::string::npos,
              "proxy location method limit should include Allow header");
    }

    void test_proxy_access_limit(uint16_t port)
    {
        const std::string resp = http_get(port, "/proxy-deny/demo");
        check(resp.find("403") != std::string::npos,
              "proxy location deny rule should return 403");
    }

    void test_proxy_exact_match(uint16_t port)
    {
        const std::string exact = http_get(port, "/proxy-exact?x=1");
        check(exact.find("200") != std::string::npos,
              "proxy exact route should match exact path with query string");

        const std::string child = http_get(port, "/proxy-exact/child");
        check(child.find("404") != std::string::npos,
              "proxy exact route should not match child paths");
    }

    void test_proxy_location_priority(uint16_t port)
    {
        const std::string normal = http_get(port, "/proxy-priority/plain.txt");
        check(normal.find("200") != std::string::npos,
              "normal proxy prefix priority route should return 200");
        check(normal.find("X-Priority-Route: normal") != std::string::npos,
              "normal prefix should be selected after exact, strong prefix, and regex checks");

        const std::string regex = http_get(port, "/proxy-priority/data.json");
        check(regex.find("200") != std::string::npos,
              "regex proxy priority route should return 200");
        check(regex.find("X-Priority-Route: regex") != std::string::npos,
              "regex route should beat a normal prefix route");

        const std::string strong = http_get(port, "/proxy-priority/strong/data.json");
        check(strong.find("200") != std::string::npos,
              "strong-prefix proxy priority route should return 200");
        check(strong.find("X-Priority-Route: strong") != std::string::npos,
              "strong prefix route should beat a matching regex route");
    }

    void test_proxy_cache(uint16_t port, const std::filesystem::path &upstream_log_path)
    {
        {
            std::ofstream clear(upstream_log_path, std::ios::binary | std::ios::trunc);
        }

        const std::string first = http_get(port, "/proxy-cache/item?id=1");
        check(first.find("200") != std::string::npos,
              "proxy cache first response should return 200");
        check(first.find("X-Cache: MISS") != std::string::npos,
              "proxy cache first response should be marked MISS");

        const std::string second = http_get(port, "/proxy-cache/item?id=1");
        check(second.find("200") != std::string::npos,
              "proxy cache second response should return 200");
        check(second.find("X-Cache: HIT") != std::string::npos,
              "proxy cache second response should be marked HIT");

        const std::string bypass = http_request(port,
                                                "GET",
                                                "/proxy-cache/item?id=1",
                                                "X-Bypass-Cache: 1\r\n");
        check(bypass.find("200") != std::string::npos,
              "proxy cache bypass response should return 200");
        check(bypass.find("X-Cache: BYPASS") != std::string::npos,
              "proxy cache bypass response should be marked BYPASS");

        const std::string still_cached = http_get(port, "/proxy-cache/item?id=1");
        check(still_cached.find("X-Cache: HIT") != std::string::npos,
              "proxy cache bypass should not replace an existing cached response");

        const std::string no_store = http_request(port,
                                                  "GET",
                                                  "/proxy-cache/no-store?id=2",
                                                  "X-No-Cache-Store: 1\r\n");
        check(no_store.find("200") != std::string::npos,
              "proxy no-cache-store response should return 200");
        check(no_store.find("X-Cache: MISS") != std::string::npos,
              "proxy no-cache-store response should still be marked MISS");

        const std::string after_no_store = http_get(port, "/proxy-cache/no-store?id=2");
        check(after_no_store.find("X-Cache: MISS") != std::string::npos,
              "proxy no-cache-store response should not populate cache");

        const std::string private_first = http_get(port, "/proxy-cache/private?id=3");
        check(private_first.find("X-Cache: MISS") != std::string::npos,
              "proxy cache should mark private upstream response as MISS");
        const std::string private_second = http_get(port, "/proxy-cache/private?id=3");
        check(private_second.find("X-Cache: MISS") != std::string::npos,
              "proxy cache should not store private upstream responses");

        std::ifstream in(upstream_log_path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string upstream_log = buffer.str();
        auto count_occurrences = [&](const std::string &needle) {
            std::size_t hits = 0;
            std::size_t pos = 0;
            while ((pos = upstream_log.find(needle, pos)) != std::string::npos) {
                ++hits;
                pos += needle.size();
            }
            return hits;
        };
        std::size_t hits = count_occurrences("GET /proxy-cache/item?id=1 HTTP/1.1");
        check(hits == 2, "proxy cache should use upstream once plus one explicit bypass for the same URI");

        hits = count_occurrences("GET /proxy-cache/no-store?id=2 HTTP/1.1");
        check(hits == 2, "proxy no-cache-store should avoid storing the first response");

        hits = count_occurrences("GET /proxy-cache/private?id=3 HTTP/1.1");
        check(hits == 2, "proxy cache should respect upstream Cache-Control private");
    }

    void test_http_caps_with_config_flags(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = true;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }

        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        const std::string caps = http_get(port, "/__http_caps");
        check(caps.find("200") != std::string::npos, "__http_caps with flags should return 200");
        check(caps.find("\"http2\":true") != std::string::npos, "__http_caps should reflect http2=true from config");
        check(caps.find("\"http3\":true") != std::string::npos, "__http_caps should reflect http3=true from config");

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
    }

    void test_http_version_gate(uint16_t port)
    {
        const std::string req_h2 =
            "GET /__http_caps HTTP/2.0\r\n"
            "Host: 127.0.0.1:" +
            std::to_string(port) + "\r\n"
                                   "Connection: close\r\n\r\n";

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2.0 gate test should connect");
        if (s == kInvalidSocket) {
            return;
        }
        if (!send_all(s, req_h2)) {
            close_socket(s);
            check(false, "http/2.0 gate test should send request");
            return;
        }
        const std::string resp_h2 = recv_all(s);
        close_socket(s);
        if (resp_h2.find("505") == std::string::npos) {
            std::cerr << "[DEBUG] http/2.0 gate response:\n"
                      << resp_h2 << "\n";
        }
        check(resp_h2.find("505") != std::string::npos,
              "HTTP/2.0 request should be rejected with 505 when protocol stack disabled");

        const std::string req_h3 =
            "GET /__http_caps HTTP/3.0\r\n"
            "Host: 127.0.0.1:" +
            std::to_string(port) + "\r\n"
                                   "Connection: close\r\n\r\n";

        s = connect_loopback(port);
        check(s != kInvalidSocket, "http/3.0 gate test should connect");
        if (s == kInvalidSocket) {
            return;
        }
        if (!send_all(s, req_h3)) {
            close_socket(s);
            check(false, "http/3.0 gate test should send request");
            return;
        }
        const std::string resp_h3 = recv_all(s);
        close_socket(s);
        if (resp_h3.find("505") == std::string::npos) {
            std::cerr << "[DEBUG] http/3.0 gate response:\n"
                      << resp_h3 << "\n";
        }
        check(resp_h3.find("505") != std::string::npos,
              "HTTP/3.0 request should be rejected with 505 when protocol stack disabled");
    }

    void test_http2_preface_gate(uint16_t port)
    {
        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 preface gate test should connect");
        if (s == kInvalidSocket) {
            return;
        }
        if (!send_all(s, preface)) {
            close_socket(s);
            check(false, "http/2 preface gate test should send preface");
            return;
        }

        const std::string resp = recv_all(s);
        close_socket(s);
        check(resp.find("505") != std::string::npos,
              "HTTP/2 connection preface should be rejected with 505 when protocol stack disabled");
    }

    void test_http2_tls_only_rejects_h2c()
    {
        const uint16_t port = reserve_tcp_port();
        check(port != 0, "http/2 tls-only test should reserve a port");
        if (port == 0) {
            return;
        }

        yuan::net::http::HttpServerConfig cfg;
        cfg.enable_http2 = true;
        cfg.http2_tls_only = true;
        yuan::server::HttpService service(port, cfg);
        if (!service.init()) {
            check(false, "http/2 tls-only service should init");
            return;
        }
        service.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 tls-only test should connect");
        if (s != kInvalidSocket) {
            const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
            check(send_all(s, preface), "http/2 tls-only test should send preface");
            const std::string resp = recv_all(s);
            close_socket(s);
            check(resp.find("505") != std::string::npos,
                  "http2_tls_only should reject cleartext h2c preface with 505");
        }

        service.stop();
    }

    void test_http2_preface_settings_ack(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 settings ack test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        if (!send_all(s, preface + settings)) {
            close_socket(s);
            check(false, "http/2 settings ack test should send preface+settings");
            return;
        }

        skip_server_settings(s);

        std::string hdr;
        std::string payload;
        const bool got = recv_http2_frame(s, hdr, payload);
        check(got, "http/2 settings ack test should receive frame");
        if (!got) {
            close_socket(s);
            return;
        }
        close_socket(s);

        const unsigned char type = static_cast<unsigned char>(hdr[3]);
        const unsigned char flags = static_cast<unsigned char>(hdr[4]);
        check(type == 0x04, "http/2 response frame should be SETTINGS");
        check((flags & 0x01) != 0, "http/2 response SETTINGS should carry ACK");

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_fragmented_preface_settings_ack(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 fragmented preface test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string first = preface.substr(0, 19);
        const std::string second = preface.substr(19) + settings;
        if (!send_all(s, first)) {
            close_socket(s);
            check(false, "http/2 fragmented preface test should send first chunk");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!send_all(s, second)) {
            close_socket(s);
            check(false, "http/2 fragmented preface test should send second chunk");
            return;
        }

        skip_server_settings(s);

        std::string hdr;
        std::string payload;
        const bool got = recv_http2_frame(s, hdr, payload);
        check(got, "http/2 fragmented preface test should receive frame");
        close_socket(s);
        if (got) {
            check(static_cast<unsigned char>(hdr[3]) == 0x04,
                  "http/2 fragmented preface response should be SETTINGS");
            check((static_cast<unsigned char>(hdr[4]) & 0x01) != 0,
                  "http/2 fragmented preface response SETTINGS should carry ACK");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_ping_ack(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 ping ack test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        std::string ping;
        ping.resize(17);
        ping[0] = 0;
        ping[1] = 0;
        ping[2] = 8;
        ping[3] = 0x06;
        ping[4] = 0x00;
        ping[5] = 0x00;
        ping[6] = 0x00;
        ping[7] = 0x00;
        ping[8] = 0x00;
        for (int i = 0; i < 8; ++i) {
            ping[9 + i] = static_cast<char>(0x30 + i);
        }

        if (!send_all(s, preface + settings + ping)) {
            close_socket(s);
            check(false, "http/2 ping ack test should send preface+settings+ping");
            return;
        }

        skip_server_settings(s);

        std::string hdr1;
        std::string payload1;
        const bool got1 = recv_http2_frame(s, hdr1, payload1);
        check(got1, "http/2 ping ack test should receive first frame");
        if (!got1) {
            close_socket(s);
            return;
        }

        std::string hdr2;
        std::string payload2;
        const bool got2 = recv_http2_frame(s, hdr2, payload2);
        check(got2, "http/2 ping ack test should receive second frame");
        close_socket(s);
        if (!got2) {
            return;
        }

        const unsigned char type1 = static_cast<unsigned char>(hdr1[3]);
        const unsigned char flags1 = static_cast<unsigned char>(hdr1[4]);
        const unsigned char type2 = static_cast<unsigned char>(hdr2[3]);
        const unsigned char flags2 = static_cast<unsigned char>(hdr2[4]);

        const bool first_is_settings_ack = (type1 == 0x04) && ((flags1 & 0x01) != 0);
        const bool second_is_ping_ack = (type2 == 0x06) && ((flags2 & 0x01) != 0) && payload2.size() == 8;
        check(first_is_settings_ack, "http/2 ping ack test first frame should be SETTINGS ACK");
        check(second_is_ping_ack, "http/2 ping ack test second frame should be PING ACK");
        if (second_is_ping_ack) {
            check(payload2 == std::string("01234567"), "http/2 ping ack should echo opaque payload");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_invalid_window_update_goaway(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 invalid WINDOW_UPDATE test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        std::string bad_window_update;
        bad_window_update.resize(12);
        bad_window_update[0] = 0;
        bad_window_update[1] = 0;
        bad_window_update[2] = 3;
        bad_window_update[3] = 0x08;
        bad_window_update[4] = 0x00;
        bad_window_update[5] = 0x00;
        bad_window_update[6] = 0x00;
        bad_window_update[7] = 0x00;
        bad_window_update[8] = 0x00;
        bad_window_update[9] = 0x01;
        bad_window_update[10] = 0x02;
        bad_window_update[11] = 0x03;

        if (!send_all(s, preface + settings + bad_window_update)) {
            close_socket(s);
            check(false, "http/2 invalid WINDOW_UPDATE test should send bytes");
            return;
        }

        skip_server_settings(s);

        std::string h1;
        std::string p1;
        const bool got1 = recv_http2_frame(s, h1, p1);
        check(got1, "http/2 invalid WINDOW_UPDATE test should receive first frame");
        if (!got1) {
            close_socket(s);
            return;
        }

        std::string h2;
        std::string p2;
        const bool got2 = recv_http2_frame(s, h2, p2);
        close_socket(s);
        check(got2, "http/2 invalid WINDOW_UPDATE test should receive GOAWAY");
        if (!got2) {
            return;
        }

        const unsigned char type1 = static_cast<unsigned char>(h1[3]);
        const unsigned char flags1 = static_cast<unsigned char>(h1[4]);
        check(type1 == 0x04 && ((flags1 & 0x01) != 0),
              "http/2 invalid WINDOW_UPDATE first frame should be SETTINGS ACK");

        const unsigned char type2 = static_cast<unsigned char>(h2[3]);
        check(type2 == 0x07, "http/2 invalid WINDOW_UPDATE second frame should be GOAWAY");
        check(p2.size() == 8, "http/2 GOAWAY payload should be 8 bytes");
        if (p2.size() == 8) {
            const std::uint32_t err =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[4])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[5])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[6])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(p2[7]));
            check(err == 0x6u, "http/2 invalid WINDOW_UPDATE should return FRAME_SIZE_ERROR(0x6)");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_invalid_rst_stream_goaway(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 invalid RST_STREAM test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        std::string bad_rst;
        bad_rst.resize(13);
        bad_rst[0] = 0;
        bad_rst[1] = 0;
        bad_rst[2] = 4;
        bad_rst[3] = 0x03;
        bad_rst[4] = 0x00;
        bad_rst[5] = 0x00;
        bad_rst[6] = 0x00;
        bad_rst[7] = 0x00;
        bad_rst[8] = 0x00;
        bad_rst[9] = 0x00;
        bad_rst[10] = 0x00;
        bad_rst[11] = 0x00;
        bad_rst[12] = 0x01;

        if (!send_all(s, preface + settings + bad_rst)) {
            close_socket(s);
            check(false, "http/2 invalid RST_STREAM test should send bytes");
            return;
        }

        skip_server_settings(s);

        std::string h1;
        std::string p1;
        const bool got1 = recv_http2_frame(s, h1, p1);
        check(got1, "http/2 invalid RST_STREAM test should receive first frame");
        if (!got1) {
            close_socket(s);
            return;
        }

        std::string h2;
        std::string p2;
        const bool got2 = recv_http2_frame(s, h2, p2);
        close_socket(s);
        check(got2, "http/2 invalid RST_STREAM test should receive GOAWAY");
        if (!got2) {
            return;
        }

        const unsigned char type1 = static_cast<unsigned char>(h1[3]);
        const unsigned char flags1 = static_cast<unsigned char>(h1[4]);
        check(type1 == 0x04 && ((flags1 & 0x01) != 0),
              "http/2 invalid RST_STREAM first frame should be SETTINGS ACK");

        const unsigned char type2 = static_cast<unsigned char>(h2[3]);
        check(type2 == 0x07, "http/2 invalid RST_STREAM second frame should be GOAWAY");
        check(p2.size() == 8, "http/2 GOAWAY payload should be 8 bytes");
        if (p2.size() == 8) {
            const std::uint32_t err =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[4])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[5])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[6])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(p2[7]));
            check(err == 0x1u, "http/2 invalid RST_STREAM should return PROTOCOL_ERROR(0x1)");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_continuation_without_headers_goaway(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 invalid CONTINUATION test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        std::string invalid_cont;
        invalid_cont.resize(13);
        invalid_cont[0] = 0;
        invalid_cont[1] = 0;
        invalid_cont[2] = 4;
        invalid_cont[3] = 0x09;
        invalid_cont[4] = 0x04;
        invalid_cont[5] = 0x00;
        invalid_cont[6] = 0x00;
        invalid_cont[7] = 0x00;
        invalid_cont[8] = 0x01;
        invalid_cont[9] = 'a';
        invalid_cont[10] = 'b';
        invalid_cont[11] = 'c';
        invalid_cont[12] = 'd';

        if (!send_all(s, preface + settings + invalid_cont)) {
            close_socket(s);
            check(false, "http/2 invalid CONTINUATION test should send bytes");
            return;
        }

        skip_server_settings(s);

        std::string h1;
        std::string p1;
        const bool got1 = recv_http2_frame(s, h1, p1);
        check(got1, "http/2 invalid CONTINUATION test should receive first frame");
        if (!got1) {
            close_socket(s);
            return;
        }

        std::string h2;
        std::string p2;
        const bool got2 = recv_http2_frame(s, h2, p2);
        close_socket(s);
        check(got2, "http/2 invalid CONTINUATION test should receive GOAWAY");
        if (!got2) {
            return;
        }

        const unsigned char type1 = static_cast<unsigned char>(h1[3]);
        const unsigned char flags1 = static_cast<unsigned char>(h1[4]);
        check(type1 == 0x04 && ((flags1 & 0x01) != 0),
              "http/2 invalid CONTINUATION first frame should be SETTINGS ACK");

        const unsigned char type2 = static_cast<unsigned char>(h2[3]);
        check(type2 == 0x07, "http/2 invalid CONTINUATION second frame should be GOAWAY");
        check(p2.size() == 8, "http/2 GOAWAY payload should be 8 bytes");
        if (p2.size() == 8) {
            const std::uint32_t err =
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[4])) << 24) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[5])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(p2[6])) << 8) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(p2[7]));
            check(err == 0x1u, "http/2 invalid CONTINUATION should return PROTOCOL_ERROR(0x1)");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_headers_continuation_data_flow(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 headers-continuation-data flow test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string header_block = hpack_headers({
            {":method", "POST"},
            {":path", "/h2-bridge"},
            {":authority", "local.test"},
        });
        const std::size_t split = header_block.size() / 2;
        const std::string headers = h2_frame(0x01, 0x00, 1, header_block.substr(0, split));
        const std::string cont = h2_frame(0x09, 0x04, 1, header_block.substr(split));

        const std::string body = "hello-h2-body";
        std::string data;
        data.resize(9 + body.size());
        data[0] = static_cast<char>((body.size() >> 16) & 0xff);
        data[1] = static_cast<char>((body.size() >> 8) & 0xff);
        data[2] = static_cast<char>(body.size() & 0xff);
        data[3] = 0x00;
        data[4] = 0x01;
        data[5] = 0x00;
        data[6] = 0x00;
        data[7] = 0x00;
        data[8] = 0x01;
        std::memcpy(data.data() + 9, body.data(), body.size());

        if (!send_all(s, preface + settings + headers + cont + data)) {
            close_socket(s);
            check(false, "http/2 headers-continuation-data flow test should send bytes");
            return;
        }

        skip_server_settings(s);

        std::string h1;
        std::string p1;
        const bool got1 = recv_http2_frame(s, h1, p1);
        close_socket(s);
        check(got1, "http/2 headers-continuation-data flow should receive SETTINGS ACK");
        if (!got1) {
            return;
        }
        const unsigned char type1 = static_cast<unsigned char>(h1[3]);
        const unsigned char flags1 = static_cast<unsigned char>(h1[4]);
        check(type1 == 0x04 && ((flags1 & 0x01) != 0),
              "http/2 headers-continuation-data flow first frame should be SETTINGS ACK");

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_minimal_response_echo(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 minimal response echo test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x04, 1, hpack_headers({
                                                                {":method", "POST"},
                                                                {":path", "/__h2_echo"},
                                                                {":authority", "local.test"},
                                                            }));

        const std::string body = "echo-body-xyz";
        std::string data;
        data.resize(9 + body.size());
        data[0] = static_cast<char>((body.size() >> 16) & 0xff);
        data[1] = static_cast<char>((body.size() >> 8) & 0xff);
        data[2] = static_cast<char>(body.size() & 0xff);
        data[3] = 0x00;
        data[4] = 0x01;
        data[5] = 0x00;
        data[6] = 0x00;
        data[7] = 0x00;
        data[8] = 0x01;
        std::memcpy(data.data() + 9, body.data(), body.size());

        if (!send_all(s, preface + settings + headers + data)) {
            close_socket(s);
            check(false, "http/2 minimal response echo test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 10, frames);
        close_socket(s);
        check(got, "http/2 minimal response echo should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_settings = static_cast<std::size_t>(-1);
        std::size_t idx_headers = static_cast<std::size_t>(-1);
        std::size_t idx_data = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto t = static_cast<unsigned char>(frames[i].first[3]);
            const auto fl = static_cast<unsigned char>(frames[i].first[4]);
            if (idx_settings == static_cast<std::size_t>(-1) && t == 0x04 && (fl & 0x01) != 0) {
                idx_settings = i;
            } else if (idx_headers == static_cast<std::size_t>(-1) && t == 0x01) {
                idx_headers = i;
            } else if (idx_data == static_cast<std::size_t>(-1) && t == 0x00) {
                idx_data = i;
            }
        }

        if (idx_settings == static_cast<std::size_t>(-1) ||
            idx_headers == static_cast<std::size_t>(-1) ||
            idx_data == static_cast<std::size_t>(-1)) {
            dump_http2_frames("h2 echo", frames);
            check(false, "http/2 minimal response echo should receive at least 3 frames");
            return;
        }

        const auto &h1 = frames[idx_settings].first;
        const auto &h2 = frames[idx_headers].first;
        const auto &h3 = frames[idx_data].first;
        const auto &p3 = frames[idx_data].second;

        const unsigned char t1 = static_cast<unsigned char>(h1[3]);
        const unsigned char f1 = static_cast<unsigned char>(h1[4]);
        const unsigned char t2 = static_cast<unsigned char>(h2[3]);
        const unsigned char f2 = static_cast<unsigned char>(h2[4]);
        const unsigned char t3 = static_cast<unsigned char>(h3[3]);
        const unsigned char f3 = static_cast<unsigned char>(h3[4]);

        check(t1 == 0x04 && ((f1 & 0x01) != 0), "first response frame should be SETTINGS ACK");
        check(t2 == 0x01, "second response frame should be HEADERS");
        check((f2 & 0x04) != 0, "response HEADERS should set END_HEADERS");
        check(t3 == 0x00, "third response frame should be DATA");
        check((f3 & 0x01) != 0, "response DATA should set END_STREAM");
        if (p3.find("\"body\":\"echo-body-xyz\"") == std::string::npos) {
            std::cerr << "[DEBUG] h2 echo DATA payload: " << p3 << "\n";
        }
        check(p3.find("\"body\":\"echo-body-xyz\"") != std::string::npos,
              "response DATA payload should include echoed json body field");

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_minimal_response_caps(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 minimal response caps test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x05, 1, hpack_headers({
                                                                {":method", "GET"},
                                                                {":path", "/__http_caps"},
                                                                {":authority", "local.test"},
                                                            }));

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 minimal response caps test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 10, frames);
        close_socket(s);
        check(got, "http/2 minimal response caps should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_settings = static_cast<std::size_t>(-1);
        std::size_t idx_headers = static_cast<std::size_t>(-1);
        std::size_t idx_data = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto t = static_cast<unsigned char>(frames[i].first[3]);
            const auto fl = static_cast<unsigned char>(frames[i].first[4]);
            if (idx_settings == static_cast<std::size_t>(-1) && t == 0x04 && (fl & 0x01) != 0) {
                idx_settings = i;
            } else if (idx_headers == static_cast<std::size_t>(-1) && t == 0x01) {
                idx_headers = i;
            } else if (idx_data == static_cast<std::size_t>(-1) && t == 0x00) {
                idx_data = i;
            }
        }

        if (idx_settings == static_cast<std::size_t>(-1) ||
            idx_headers == static_cast<std::size_t>(-1) ||
            idx_data == static_cast<std::size_t>(-1)) {
            dump_http2_frames("h2 caps", frames);
            check(false, "http/2 minimal response caps should receive at least 3 frames");
            return;
        }

        const auto &h1 = frames[idx_settings].first;
        const auto &h2 = frames[idx_headers].first;
        const auto &h3 = frames[idx_data].first;
        const auto &p3 = frames[idx_data].second;

        check(static_cast<unsigned char>(h1[3]) == 0x04, "caps first frame should be SETTINGS ACK");
        check(static_cast<unsigned char>(h2[3]) == 0x01, "caps second frame should be HEADERS");
        check(static_cast<unsigned char>(h3[3]) == 0x00, "caps third frame should be DATA");
        check((static_cast<unsigned char>(h3[4]) & 0x01) != 0,
              "caps DATA should set END_STREAM");
        check(p3.find("\"http2\":true") != std::string::npos,
              "caps data should indicate http2 enabled");

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_large_response_data_split(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 large response test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x05, 1, hpack_headers({
                                                                {":method", "GET"},
                                                                {":path", "/__h2_large"},
                                                                {":authority", "local.test"},
                                                            }));

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 large response test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 16, frames);
        close_socket(s);
        check(got, "http/2 large response test should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_headers = static_cast<std::size_t>(-1);
        std::size_t data_frames = 0;
        std::size_t data_bytes = 0;
        bool saw_end_stream = false;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto t = static_cast<unsigned char>(frames[i].first[3]);
            const auto fl = static_cast<unsigned char>(frames[i].first[4]);
            if (idx_headers == static_cast<std::size_t>(-1) && t == 0x01) {
                idx_headers = i;
            } else if (t == 0x00) {
                ++data_frames;
                data_bytes += frames[i].second.size();
                saw_end_stream = saw_end_stream || ((fl & 0x01) != 0);
            }
        }

        check(idx_headers != static_cast<std::size_t>(-1), "http/2 large response should include HEADERS");
        if (idx_headers != static_cast<std::size_t>(-1)) {
            const auto status = h2_header_value(frames[idx_headers].second, ":status");
            check(status && *status == "200", "http/2 large response should return 200");
        }
        check(data_frames >= 2, "http/2 large response should be split into multiple DATA frames");
        check(data_bytes == 60000, "http/2 large response should deliver full body");
        check(saw_end_stream, "http/2 large response final DATA should set END_STREAM");

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_window_update_flushes_pending_data(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 WINDOW_UPDATE pending data test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x05, 1, hpack_headers({
                                                                {":method", "GET"},
                                                                {":path", "/__h2_window"},
                                                                {":authority", "local.test"},
                                                            }));

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 WINDOW_UPDATE pending data test should send bytes");
            return;
        }

        std::size_t data_bytes = 0;
        std::size_t data_frames = 0;
        bool sent_update = false;
        bool saw_end_stream = false;
        bool saw_headers = false;

        for (int i = 0; i < 20 && !saw_end_stream; ++i) {
            std::string hdr;
            std::string payload;
            if (!recv_http2_frame(s, hdr, payload)) {
                break;
            }
            const auto type = static_cast<unsigned char>(hdr[3]);
            const auto flags = static_cast<unsigned char>(hdr[4]);
            if (type == 0x01) {
                saw_headers = true;
                const auto status = h2_header_value(payload, ":status");
                check(status && *status == "200", "http/2 WINDOW_UPDATE pending data should return 200");
            } else if (type == 0x00) {
                ++data_frames;
                data_bytes += payload.size();
                saw_end_stream = (flags & 0x01) != 0;
                if (!sent_update) {
                    const std::string update = h2_window_update(0, 10000) + h2_window_update(1, 10000);
                    if (!send_all(s, update)) {
                        check(false, "http/2 WINDOW_UPDATE pending data test should send WINDOW_UPDATE");
                        break;
                    }
                    sent_update = true;
                }
            }
        }
        close_socket(s);

        check(saw_headers, "http/2 WINDOW_UPDATE pending data should receive HEADERS");
        check(sent_update, "http/2 WINDOW_UPDATE pending data should send window increments");
        check(data_frames >= 5, "http/2 WINDOW_UPDATE pending data should receive multiple DATA frames");
        check(data_bytes == 70000, "http/2 WINDOW_UPDATE pending data should deliver full body");
        check(saw_end_stream, "http/2 WINDOW_UPDATE pending data should end stream");

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_unknown_path_404(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 unknown path test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x05, 1, hpack_headers({
                                                                {":method", "GET"},
                                                                {":path", "/__h2_unknown_path"},
                                                                {":authority", "local.test"},
                                                            }));

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 unknown path test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 8, frames);
        close_socket(s);
        check(got, "http/2 unknown path test should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_headers = static_cast<std::size_t>(-1);
        std::size_t idx_data = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto t = static_cast<unsigned char>(frames[i].first[3]);
            if (idx_headers == static_cast<std::size_t>(-1) && t == 0x01) {
                idx_headers = i;
            } else if (idx_data == static_cast<std::size_t>(-1) && t == 0x00) {
                idx_data = i;
            }
        }

        check(idx_headers != static_cast<std::size_t>(-1), "http/2 unknown path should include HEADERS frame");
        if (idx_headers != static_cast<std::size_t>(-1)) {
            const auto status = h2_header_value(frames[idx_headers].second, ":status");
            check(status && *status == "404",
                  "http/2 unknown path should return 404 status in headers");
        }
        if (idx_data != static_cast<std::size_t>(-1)) {
            const auto &data_payload_resp = frames[idx_data].second;
            check(data_payload_resp.find("not found") != std::string::npos,
                  "http/2 unknown path should return not found body");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_configurable_dispatch_path(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        cfg["h2_dispatch_paths"] = nlohmann::json::array({"/__h2_extra"});
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        const std::string reload_resp = http_get(port, "/reload_config");
        check(reload_resp.find("200") != std::string::npos, "reload_config should return 200 for configurable h2 path test");

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 configurable path test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x05, 1, hpack_headers({
                                                                {":method", "GET"},
                                                                {":path", "/__h2_extra"},
                                                                {":authority", "local.test"},
                                                            }));

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 configurable path test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 6, frames);
        close_socket(s);
        check(got, "http/2 configurable path test should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_headers = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto t = static_cast<unsigned char>(frames[i].first[3]);
            if (t == 0x01) {
                idx_headers = i;
                break;
            }
        }
        if (idx_headers != static_cast<std::size_t>(-1)) {
            const auto status = h2_header_value(frames[idx_headers].second, ":status");
            check(status && *status == "404",
                  "http/2 configurable path should be dispatched and keep 404 semantics");
        } else {
            std::size_t idx_goaway = static_cast<std::size_t>(-1);
            for (std::size_t i = 0; i < frames.size(); ++i) {
                if (static_cast<unsigned char>(frames[i].first[3]) == 0x07) {
                    idx_goaway = i;
                    break;
                }
            }
            check(idx_goaway != static_cast<std::size_t>(-1),
                  "http/2 configurable path should either return headers or terminate with GOAWAY");
        }

        const std::string reload_back = http_get(port, "/reload_config");
        check(reload_back.find("200") != std::string::npos, "reload_config should keep service config consistent after configurable h2 path test");
    }

    void test_http2_invalid_pseudo_headers_400(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 invalid pseudo-headers test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x05, 1, hpack_headers({
                                                                {":method", "GET"},
                                                                {":authority", "local.test"},
                                                            }));

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 invalid pseudo-headers test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 6, frames);
        close_socket(s);
        check(got, "http/2 invalid pseudo-headers test should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_headers = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (static_cast<unsigned char>(frames[i].first[3]) == 0x01) {
                idx_headers = i;
                break;
            }
        }
        check(idx_headers != static_cast<std::size_t>(-1), "http/2 invalid pseudo-headers should return HEADERS response");
        if (idx_headers != static_cast<std::size_t>(-1)) {
            const auto status = h2_header_value(frames[idx_headers].second, ":status");
            check(status && *status == "400",
                  "http/2 invalid pseudo-headers should return 400 status");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_pseudo_after_regular_header_400(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 pseudo-after-regular test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        const std::string headers = h2_frame(0x01, 0x05, 1, hpack_headers({
                                                                {"x-test", "1"},
                                                                {":method", "GET"},
                                                                {":path", "/__http_caps"},
                                                            }));

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 pseudo-after-regular test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 6, frames);
        close_socket(s);
        check(got, "http/2 pseudo-after-regular test should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_headers = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (static_cast<unsigned char>(frames[i].first[3]) == 0x01) {
                idx_headers = i;
                break;
            }
        }
        check(idx_headers != static_cast<std::size_t>(-1), "http/2 pseudo-after-regular should return HEADERS response");
        if (idx_headers != static_cast<std::size_t>(-1)) {
            const auto status = h2_header_value(frames[idx_headers].second, ":status");
            check(status && *status == "400",
                  "http/2 pseudo-after-regular should return 400 status");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_http2_hpack_indexed_headers_flow(uint16_t port)
    {
        nlohmann::json cfg;
        cfg["enable_http2"] = true;
        cfg["enable_http3"] = false;
        {
            std::ofstream out("http.json", std::ios::binary);
            out << cfg.dump(2);
        }
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "http/2 HPACK indexed flow test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        settings.resize(9);
        settings[0] = 0;
        settings[1] = 0;
        settings[2] = 0;
        settings[3] = 0x04;
        settings[4] = 0x00;
        settings[5] = 0x00;
        settings[6] = 0x00;
        settings[7] = 0x00;
        settings[8] = 0x00;

        std::string hpack;
        hpack_append_indexed(hpack, 2);                                // :method: GET
        hpack_append_indexed(hpack, 4);                                // :path: /
        hpack_append_literal_without_indexing(hpack, 1, "local.test"); // :authority

        std::string headers;
        headers.resize(9 + hpack.size());
        headers[0] = static_cast<char>((hpack.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((hpack.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(hpack.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x05;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, hpack.data(), hpack.size());

        if (!send_all(s, preface + settings + headers)) {
            close_socket(s);
            check(false, "http/2 HPACK indexed flow test should send bytes");
            return;
        }

        std::vector<std::pair<std::string, std::string>> frames;
        const bool got = recv_http2_frames(s, 6, frames);
        close_socket(s);
        check(got, "http/2 HPACK indexed flow test should receive frames");
        if (!got) {
            return;
        }

        std::size_t idx_headers = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (static_cast<unsigned char>(frames[i].first[3]) == 0x01) {
                idx_headers = i;
                break;
            }
        }
        check(idx_headers != static_cast<std::size_t>(-1), "http/2 HPACK indexed flow should return HEADERS response");
        if (idx_headers != static_cast<std::size_t>(-1)) {
            const auto status = h2_header_value(frames[idx_headers].second, ":status");
            check(status && *status == "404",
                  "http/2 HPACK indexed flow should decode pseudo headers and return 404 for '/'");
        }

        std::error_code ec;
        std::filesystem::remove("http.json", ec);
        (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
        yuan::net::http::config::load_config();
    }

    void test_static_etag_and_304(uint16_t port, const std::filesystem::path &root)
    {
        const auto file = root / "etag.txt";
        {
            std::ofstream out(file, std::ios::binary);
            out << "hello etag test";
        }

        const std::string first = http_get(port, "/static/etag.txt");
        check(first.find("200") != std::string::npos, "etag first fetch should be 200");
        const auto etag_pos = first.find("etag: ");
        check(etag_pos != std::string::npos, "etag header should exist");
        if (etag_pos == std::string::npos) {
            return;
        }
        const auto etag_end = first.find("\r\n", etag_pos);
        const std::string etag = trim_http_value(first.substr(etag_pos + 6, etag_end - (etag_pos + 6)));

        const auto lm_pos = first.find("last-modified: ");
        check(lm_pos != std::string::npos, "last-modified header should exist");
        if (lm_pos == std::string::npos) {
            return;
        }
        const auto lm_end = first.find("\r\n", lm_pos);
        const std::string last_modified = trim_http_value(first.substr(lm_pos + 15, lm_end - (lm_pos + 15)));
        (void)last_modified;

        const std::string second = http_get(port,
                                            "/static/etag.txt",
                                            "If-None-Match: " + etag + "\r\n"
                                                                       "If-Modified-Since: Thu, 31 Dec 2099 23:59:59 GMT\r\n");
        if (second.find("304") == std::string::npos) {
            std::cerr << "[DEBUG] conditional response:\n"
                      << second << "\n";
        }
        check(second.find("304") != std::string::npos,
              "conditional fetch with validators should be 304");
    }

    void test_static_gzip(uint16_t port, const std::filesystem::path &root)
    {
        const auto file = root / "gzip.txt";
        {
            std::ofstream out(file, std::ios::binary);
            for (int i = 0; i < 2048; ++i) {
                out << "gzip-compressible-line-" << (i % 7) << '\n';
            }
        }

        const std::string resp = http_get(port, "/static/gzip.txt", "Accept-Encoding: gzip\r\n");
        check(resp.find("200") != std::string::npos, "gzip static fetch should be 200");
#if YUAN_HTTP_HAS_ZLIB
        if (resp.find("content-encoding: gzip") == std::string::npos) {
            std::cerr << "[DEBUG] gzip response:\n"
                      << resp << "\n";
        }
        check(resp.find("content-encoding: gzip") != std::string::npos, "gzip response should include content-encoding");
        check(resp.find("vary: accept-encoding") != std::string::npos, "gzip response should include vary header");
#else
        check(resp.find("content-encoding: br") != std::string::npos,
              "without zlib, gzip request should prefer br when brotli exists");
#endif
    }

    void test_static_precompressed_br(uint16_t port, const std::filesystem::path &root)
    {
        const auto src = root / "precompressed.txt";
        const auto br = root / "precompressed.txt.br";
        {
            std::ofstream out(src, std::ios::binary);
            out << "plain fallback content";
        }
        {
            std::ofstream out(br, std::ios::binary);
            out << "fake-br-payload";
        }

        const std::string resp = http_get(port, "/static/precompressed.txt", "Accept-Encoding: br\r\n");
        check(resp.find("200") != std::string::npos, "precompressed br fetch should be 200");
        if (resp.find("content-encoding: br") == std::string::npos) {
            std::cerr << "[DEBUG] precompressed br response:\n"
                      << resp << "\n";
        }
        check(resp.find("content-encoding: br") != std::string::npos, "precompressed br should set br encoding");
        check(resp.find("fake-br-payload") != std::string::npos, "precompressed br should serve .br payload");
    }

    void test_static_mime_and_gzip_types(uint16_t port, const std::filesystem::path &root)
    {
        {
            std::ofstream out(root / "style.CSS", std::ios::binary);
            out << "body { color: #123456; }\n";
        }
        {
            std::ofstream out(root / "module.mjs", std::ios::binary);
            out << "export const ok = true;\n";
        }
        {
            std::ofstream out(root / "app.wasm", std::ios::binary);
            out << "fake-wasm";
        }
        {
            std::ofstream out(root / "data.foo", std::ios::binary);
            for (int i = 0; i < 2048; ++i) {
                out << "custom-compressible-type-" << (i % 5) << '\n';
            }
        }
        {
            std::ofstream out(root / "blob.unknown", std::ios::binary);
            out << "unknown-default";
        }

        const std::string css = http_get(port, "/static/style.CSS");
        check(css.find("200") != std::string::npos, "css static fetch should be 200");
        check(contains_header_fragment(css, "content-type: text/css"),
              "css content type should be text/css");

        const std::string mjs = http_get(port, "/static/module.mjs");
        check(contains_header_fragment(mjs, "content-type: text/javascript"),
              "mjs content type should be text/javascript");

        const std::string wasm = http_get(port, "/static/app.wasm");
        check(contains_header_fragment(wasm, "content-type: application/wasm"),
              "wasm content type should be application/wasm");

        const std::string custom = http_get(port, "/typed/data.foo");
        check(contains_header_fragment(custom, "content-type: application/x-test"),
              "custom static types map should override MIME type");

        const std::string fallback = http_get(port, "/typed/blob.unknown");
        check(contains_header_fragment(fallback, "content-type: application/x-default"),
              "static default_type should apply to unknown extensions");

        const std::string compressed = http_get(port, "/typed/data.foo", "Accept-Encoding: gzip\r\n");
#if YUAN_HTTP_HAS_ZLIB
        if (compressed.find("content-encoding: gzip") == std::string::npos) {
            std::cerr << "[DEBUG] custom gzip_types response:\n"
                      << compressed << "\n";
        }
        check(compressed.find("content-encoding: gzip") != std::string::npos,
              "gzip_types should allow compression for custom MIME types");
        check(compressed.find("vary: accept-encoding") == std::string::npos,
              "gzip_vary=false should suppress Vary header");
#else
        (void)compressed;
#endif
    }

    void test_static_gzip_policy(uint16_t port, const std::filesystem::path &root)
    {
        {
            std::ofstream out(root / "policy.txt", std::ios::binary);
            for (int i = 0; i < 2048; ++i) {
                out << "policy-compressible-line-" << (i % 11) << '\n';
            }
        }

        const std::string normal = http_get(port, "/gzip-policy/policy.txt", "Accept-Encoding: gzip\r\n");
        check(normal.find("200") != std::string::npos, "gzip policy normal fetch should be 200");
#if YUAN_HTTP_HAS_ZLIB
        check(normal.find("content-encoding: gzip") != std::string::npos,
              "gzip policy normal request should compress");

        const std::string disabled_ua = http_get(port,
                                                 "/gzip-policy/policy.txt",
                                                 "Accept-Encoding: gzip\r\n"
                                                 "User-Agent: BadBot/1.0\r\n");
        check(disabled_ua.find("content-encoding:") == std::string::npos,
              "gzip_disable should suppress compression for matching user-agent");

        const std::string http10 = raw_http_request(
            port,
            "GET /gzip-policy/policy.txt HTTP/1.0\r\n"
            "Host: 127.0.0.1:" +
                std::to_string(port) + "\r\n"
                                       "Accept-Encoding: gzip\r\n"
                                       "Connection: close\r\n\r\n");
        check(http10.find("200") != std::string::npos, "gzip policy HTTP/1.0 fetch should be 200");
        check(http10.find("content-encoding:") == std::string::npos,
              "gzip_http_version should suppress compression below configured version");

        const std::string proxied_no_auth = http_get(port,
                                                     "/gzip-policy/policy.txt",
                                                     "Accept-Encoding: gzip\r\n"
                                                     "Via: 1.1 proxy\r\n");
        check(proxied_no_auth.find("content-encoding:") == std::string::npos,
              "gzip_proxied should suppress proxied requests that do not match a condition");

        const std::string proxied_auth = http_get(port,
                                                  "/gzip-policy/policy.txt",
                                                  "Accept-Encoding: gzip\r\n"
                                                  "Via: 1.1 proxy\r\n"
                                                  "Authorization: Basic dGVzdDpzZWNyZXQ=\r\n");
        check(proxied_auth.find("content-encoding: gzip") != std::string::npos,
              "gzip_proxied auth should allow proxied requests with Authorization");
#else
        (void)normal;
#endif
    }

    void test_static_method_limit(uint16_t port, const std::filesystem::path &root)
    {
        {
            std::ofstream out(root / "readonly.txt", std::ios::binary);
            out << "readonly-ok";
        }

        const std::string get_resp = http_get(port, "/readonly/readonly.txt");
        check(get_resp.find("200") != std::string::npos,
              "static method-limited mount should allow GET");

        const std::string post_resp = http_request(port, "POST", "/readonly/readonly.txt", {}, "blocked");
        check(post_resp.find("405") != std::string::npos,
              "static method-limited mount should reject POST");
        check(post_resp.find("Allow:") != std::string::npos &&
                  post_resp.find("GET") != std::string::npos &&
                  post_resp.find("HEAD") != std::string::npos,
              "static method-limited mount should include Allow header");
    }

    void test_static_access_limit(uint16_t port, const std::filesystem::path &root)
    {
        {
            std::ofstream out(root / "blocked.txt", std::ios::binary);
            out << "blocked";
        }

        const std::string resp = http_get(port, "/blocked/blocked.txt");
        check(resp.find("403") != std::string::npos,
              "static deny rule should return 403");
    }

    void test_static_exact_match(uint16_t port)
    {
        const std::string exact = http_get(port, "/exact-static");
        check(exact.find("200") != std::string::npos,
              "static exact mount should match exact path");

        const std::string child = http_get(port, "/exact-static/child");
        check(child.find("404") != std::string::npos,
              "static exact mount should not match child paths");
    }

    void test_static_regex_location_priority(uint16_t port)
    {
        const std::string normal = http_get(port, "/static-priority/plain.txt");
        check(normal.find("200") != std::string::npos,
              "static normal prefix priority route should return 200");
        check(normal.find("X-Static-Route: normal") != std::string::npos,
              "static normal prefix should handle non-regex paths");

        const std::string regex = http_get(port, "/static-priority/data.json");
        check(regex.find("200") != std::string::npos,
              "static regex priority route should return 200");
        check(regex.find("X-Static-Route: regex") != std::string::npos,
              "static regex route should beat a normal prefix route");

        const std::string strong = http_get(port, "/static-priority/strong/data.json");
        check(strong.find("200") != std::string::npos,
              "static strong-prefix priority route should return 200");
        check(strong.find("X-Static-Route: strong") != std::string::npos,
              "static strong prefix route should beat a matching regex route");

        const std::string fallback = http_get(port, "/regex-only/file.txt");
        check(fallback.find("200") != std::string::npos,
              "static regex-only route should dispatch without a prefix mount");
        check(fallback.find("X-Static-Route: regex-only") != std::string::npos,
              "static regex-only route should use the regex mount");
    }

    void test_keep_alive_client_close_releases_connection()
    {
        const uint16_t port = reserve_tcp_port();
        check(port != 0, "keep-alive close test should reserve a port");
        if (port == 0) {
            return;
        }

        const auto root = std::filesystem::current_path() / "test_http_keep_alive_close";
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        {
            std::ofstream out(root / "ok.txt", std::ios::binary | std::ios::trunc);
            out << "keep-alive-ok";
        }

        yuan::net::http::HttpServerConfig cfg;
        cfg.enable_keep_alive = true;
        yuan::server::HttpService service(port, cfg);
        if (!service.init()) {
            check(false, "keep-alive close test service should init");
            return;
        }
        service.server().mount_static("/static", root.string());
        service.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "keep-alive close test should connect");
        if (s != kInvalidSocket) {
            const std::string req =
                "GET /static/ok.txt HTTP/1.1\r\n"
                "Host: 127.0.0.1:" +
                std::to_string(port) + "\r\n"
                                       "Connection: keep-alive\r\n\r\n";
            check(send_all(s, req), "keep-alive close test should send request");
            const std::string resp = recv_all(s);
            check(resp.find("200") != std::string::npos,
                  "keep-alive static response should be 200");
            check(resp.find("keep-alive-ok") != std::string::npos,
                  "keep-alive static response should include body");
            check(wait_http_connections(service, 1, std::chrono::milliseconds(1500)),
                  "keep-alive connection should remain active before client close");
            abortive_close_socket(s);
            check(wait_http_connections(service, 0, std::chrono::milliseconds(2500)),
                  "keep-alive client close should release connection before idle timeout");
        }

        service.stop();
        std::filesystem::remove_all(root, ec);
    }

    void test_static_stream_stalled_reader_uses_write_timeout()
    {
        const uint16_t port = reserve_tcp_port();
        check(port != 0, "stalled static stream test should reserve a port");
        if (port == 0) {
            return;
        }

        const auto root = std::filesystem::current_path() / "test_http_stalled_static";
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        const auto file = root / "stall-video.mp4";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            std::string chunk(256 * 1024, 's');
            for (int i = 0; i < 256; ++i) {
                out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            }
        }

        yuan::net::http::HttpServerConfig cfg;
        cfg.enable_keep_alive = true;
        cfg.write_timeout_ms = 300;
        yuan::server::HttpService service(port, cfg);
        if (!service.init()) {
            check(false, "stalled static stream test service should init");
            return;
        }
        service.server().mount_static("/static", root.string());
        service.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        socket_t s = connect_loopback(port, 4096);
        check(s != kInvalidSocket, "stalled static stream test should connect");
        if (s != kInvalidSocket) {
            const std::string req =
                "GET /static/stall-video.mp4 HTTP/1.1\r\n"
                "Host: 127.0.0.1:" +
                std::to_string(port) + "\r\n"
                                       "Connection: keep-alive\r\n\r\n";
            check(send_all(s, req), "stalled static stream test should send request");

            std::string resp;
            check(recv_until_http_headers(s, resp), "stalled static stream test should receive headers");
            check(resp.find("200") != std::string::npos, "stalled static stream response should be 200");
            check(wait_http_connections(service, 1, std::chrono::milliseconds(1500)),
                  "stalled static stream should be active before write timeout");

            const bool released = wait_http_connections(service, 0, std::chrono::milliseconds(5000));
            if (!released) {
                const auto stats = service.server().snapshot_server_stats();
                std::cerr << "[DEBUG] stalled static stream left active_http_connections="
                          << stats.active_http_connections << '\n';
            }
            check(released, "stalled static stream should close on write timeout before idle timeout");
            close_socket(s);
        }

        service.stop();
        std::filesystem::remove_all(root, ec);
    }

    void test_static_stream_client_abort_releases_connection(uint16_t port,
                                                             const std::filesystem::path &root,
                                                             yuan::server::HttpService &service)
    {
        const auto file = root / "large-video.mp4";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            std::string chunk(256 * 1024, '\0');
            for (std::size_t i = 0; i < chunk.size(); ++i) {
                chunk[i] = static_cast<char>('A' + (i % 23));
            }
            for (int i = 0; i < 256; ++i) {
                out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            }
        }

        socket_t s = connect_loopback(port);
        check(s != kInvalidSocket, "static stream abort test should connect");
        if (s == kInvalidSocket) {
            return;
        }

        const std::string req =
            "GET /static/large-video.mp4 HTTP/1.1\r\n"
            "Host: 127.0.0.1:" +
            std::to_string(port) + "\r\n"
                                   "Connection: keep-alive\r\n\r\n";
        check(send_all(s, req), "static stream abort test should send request");

        std::string resp;
        check(recv_until_http_headers(s, resp), "static stream abort test should receive headers");
        check(resp.find("200") != std::string::npos, "large static response should be 200");
        const auto header_end = resp.find("\r\n\r\n");
        const std::size_t content_length = header_end == std::string::npos
                                               ? static_cast<std::size_t>(-1)
                                               : parse_content_length(resp.substr(0, header_end + 4));
        check(content_length == 67108864,
              "large static response should advertise full content length");

        check(wait_http_connections(service, 1, std::chrono::milliseconds(1500)),
              "large static stream should be active before client abort");
        abortive_close_socket(s);

        const bool released = wait_http_connections(service, 0, std::chrono::milliseconds(2500));
        if (!released) {
            const auto stats = service.server().snapshot_server_stats();
            std::cerr << "[DEBUG] static stream abort left active_http_connections="
                      << stats.active_http_connections << '\n';
        }
        check(released, "static stream client abort should release connection before idle timeout");
    }

    void test_http_service_shared_runtime()
    {
        const uint16_t port = reserve_tcp_port();
        check(port != 0, "shared-runtime HTTP service should reserve a port");
        if (port == 0) {
            return;
        }

        yuan::net::NetworkRuntime runtime;
        auto bus = std::make_shared<yuan::eventbus::EventBus>();
        std::optional<yuan::server::ServiceRuntimeEvent> activated;
        std::optional<yuan::server::ServiceRuntimeEvent> stopped;

        bus->subscribe(yuan::server::events::service_activated,
                       [&](const yuan::eventbus::Event &event) {
                           if (const auto *payload = std::any_cast<yuan::server::ServiceRuntimeEvent>(&event.payload)) {
                               activated = *payload;
                           }
                       });
        bus->subscribe(yuan::server::events::service_stopped,
                       [&](const yuan::eventbus::Event &event) {
                           if (const auto *payload = std::any_cast<yuan::server::ServiceRuntimeEvent>(&event.payload)) {
                               stopped = *payload;
                           }
                       });

        yuan::app::RuntimeContext context;
        context.app_name = "http-shared-runtime-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 8;
        context.runtime_worker_count = 4;
        context.worker_index = 1;
        context.active_service_name = "http";
        context.service_index = 2;
        context.service_instance_index = 1;
        context.service_instance_count = 4;
        context.shared_runtime = &runtime;
        context.event_bus = bus;

        yuan::net::http::HttpServerConfig cfg;
        cfg.enable_keep_alive = false;
        yuan::server::HttpService service(port, cfg);
        service.set_runtime_context(context);
        const bool service_ready = service.init();
        check(service_ready, "shared-runtime HTTP service should init");
        if (!service_ready) {
            return;
        }
        service.server().on("/__shared_runtime_ping", [](yuan::net::http::HttpRequest *,
                                                         yuan::net::http::HttpResponse *resp) {
            const std::string body = "shared-runtime-ok";
            resp->set_response_code(yuan::net::http::ResponseCode::ok_);
            resp->add_header("Content-Type", "text/plain");
            resp->add_header("Content-Length", std::to_string(body.size()));
            resp->append_body(body);
            resp->send();
        });

        service.start();
        check(activated.has_value(), "shared-runtime HTTP service should publish activated event");
        if (activated) {
            check(activated->runtime_worker_count == 4 &&
                      activated->worker_index == 1 &&
                      activated->service_instance_index == 1 &&
                      activated->service_instance_count == 4,
                  "shared-runtime HTTP service event should carry worker/service identity");
        }

        std::atomic_bool runtime_entered{false};
        std::thread runtime_thread([&]() {
            runtime_entered.store(true, std::memory_order_release);
            runtime.run();
        });

        std::string resp;
        for (int i = 0; i < 80; ++i) {
            resp = http_get(port, "/__shared_runtime_ping");
            if (resp.find("shared-runtime-ok") != std::string::npos) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        check(runtime_entered.load(std::memory_order_acquire), "shared runtime should enter run loop");
        check(resp.find("200") != std::string::npos, "shared-runtime HTTP service should return 200");
        check(resp.find("shared-runtime-ok") != std::string::npos,
              "shared-runtime HTTP service should process a real request");

        service.stop();
        runtime.stop();
        if (runtime_thread.joinable()) {
            runtime_thread.join();
        }

        check(stopped.has_value(), "shared-runtime HTTP service should publish stopped event");
        if (stopped) {
            check(stopped->service_instance_index == 1 &&
                      stopped->service_instance_count == 4,
                  "shared-runtime HTTP stopped event should preserve service identity");
        }
    }

    void test_http_service_bootstrap_in_process_worker_runtime()
    {
        const uint16_t port = reserve_tcp_port();
        check(port != 0, "bootstrap in-process HTTP service should reserve a port");
        if (port == 0) {
            return;
        }

        yuan::app::RuntimeContext context;
        context.app_name = "http-bootstrap-worker-runtime-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 2;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application application(context);
        yuan::app::ServiceDescriptor descriptor;
        descriptor.name = "http";
        descriptor.type_name = "yuan::server::HttpService";
        descriptor.contract_id = "server.http";
        descriptor.contract_version = 1;
        descriptor.placement.mode = yuan::app::PlacementMode::singleton;
        descriptor.endpoints.push_back(yuan::app::ServiceEndpoint{
            "http",
            "127.0.0.1",
            port,
            "tcp"});

        yuan::net::http::HttpServerConfig cfg;
        cfg.enable_keep_alive = false;
        if (!application.add_service(descriptor, [port, cfg]() {
                auto service = std::make_shared<yuan::server::HttpService>(port, cfg);
                service->set_server_configurator([](yuan::server::HttpService &http_service) {
                    http_service.server().on("/__bootstrap_worker_ping",
                                             [](yuan::net::http::HttpRequest *,
                                                yuan::net::http::HttpResponse *resp) {
                                                 const std::string body = "bootstrap-worker-ok";
                                                 resp->set_response_code(yuan::net::http::ResponseCode::ok_);
                                                 resp->add_header("Content-Type", "text/plain");
                                                 resp->add_header("Content-Length", std::to_string(body.size()));
                                                 resp->append_body(body);
                                                 resp->send();
                                             });
                    return true;
                });
                return service;
            })) {
            check(false, "bootstrap in-process HTTP service should register factory");
            return;
        }

        yuan::app::Bootstrap bootstrap(application);
        check(bootstrap.run(), "bootstrap should start HTTP service on in-process worker runtime");
        if (!bootstrap.has_running_workers()) {
            check(false, "bootstrap should report a running in-process HTTP worker");
            bootstrap.shutdown();
            return;
        }

        std::string resp;
        for (int i = 0; i < 80; ++i) {
            resp = http_get(port, "/__bootstrap_worker_ping");
            if (resp.find("bootstrap-worker-ok") != std::string::npos) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        if (resp.find("bootstrap-worker-ok") == std::string::npos) {
            std::cerr << "[DEBUG] bootstrap worker HTTP response:\n"
                      << resp << "\n";
        }
        check(resp.find("200") != std::string::npos,
              "bootstrap in-process HTTP worker should return 200");
        check(resp.find("bootstrap-worker-ok") != std::string::npos,
              "bootstrap in-process HTTP worker should process real request");

        bootstrap.shutdown();
        check(!bootstrap.has_running_workers(),
              "bootstrap shutdown should stop in-process HTTP worker runtime");
    }
}

int main()
{
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    std::error_code cleanup_ec;
    std::filesystem::remove("http.json", cleanup_ec);
    std::filesystem::remove("mini_nginx_proxy_test_upstream.log", cleanup_ec);
    (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
    yuan::net::http::config::load_config();

    test_hpack_decoder_regressions();
    test_http_service_shared_runtime();
    test_http_service_bootstrap_in_process_worker_runtime();
    test_keep_alive_client_close_releases_connection();
    test_static_stream_stalled_reader_uses_write_timeout();

    const uint16_t port = reserve_tcp_port();
    check(port != 0, "should reserve test server port");
    if (port == 0) {
        return 1;
    }

    const auto static_root = std::filesystem::current_path() / "test_http_static";
    std::error_code ec;
    std::filesystem::create_directories(static_root, ec);
    {
        std::ofstream out(static_root / "index.html", std::ios::binary | std::ios::trunc);
        out << "static-index";
    }
    std::filesystem::create_directories(static_root / "normal-root", ec);
    std::filesystem::create_directories(static_root / "strong-root", ec);
    std::filesystem::create_directories(static_root / "static-priority", ec);
    std::filesystem::create_directories(static_root / "static-priority" / "strong", ec);
    std::filesystem::create_directories(static_root / "regex-only", ec);
    {
        std::ofstream out(static_root / "normal-root" / "plain.txt", std::ios::binary | std::ios::trunc);
        out << "normal-static";
    }
    {
        std::ofstream out(static_root / "normal-root" / "data.json", std::ios::binary | std::ios::trunc);
        out << "normal-json";
    }
    {
        std::ofstream out(static_root / "strong-root" / "data.json", std::ios::binary | std::ios::trunc);
        out << "strong-json";
    }
    {
        std::ofstream out(static_root / "static-priority" / "data.json", std::ios::binary | std::ios::trunc);
        out << "regex-json";
    }
    {
        std::ofstream out(static_root / "static-priority" / "strong" / "data.json", std::ios::binary | std::ios::trunc);
        out << "regex-strong-json";
    }
    {
        std::ofstream out(static_root / "regex-only" / "file.txt", std::ios::binary | std::ios::trunc);
        out << "regex-only";
    }

    yuan::net::http::HttpServerConfig cfg;
    cfg.enable_keep_alive = false;
    cfg.write_timeout_ms = 800;

    const uint16_t upstream_port = reserve_tcp_port();
    check(upstream_port != 0, "should reserve proxy upstream test port");
    if (upstream_port == 0) {
        return 1;
    }
    const uint16_t closed_proxy_port = reserve_tcp_port();
    check(closed_proxy_port != 0, "should reserve closed proxy failure test port");
    if (closed_proxy_port == 0) {
        return 1;
    }

    const auto upstream_log_path = std::filesystem::current_path() / "mini_nginx_proxy_test_upstream.log";
    {
        std::ofstream clear_log(upstream_log_path, std::ios::binary | std::ios::trunc);
    }

    std::atomic_bool upstream_ready{false};
    std::atomic_bool upstream_stop{false};
    std::thread upstream_thread([upstream_port, &upstream_ready, &upstream_stop, upstream_log_path]() {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return;
        }

        int reuse = 1;
#ifdef _WIN32
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(upstream_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener, 16) != 0) {
            close_socket(listener);
            return;
        }

#ifdef _WIN32
        const DWORD accept_timeout_ms = 500;
        (void)::setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char *>(&accept_timeout_ms),
                           sizeof(accept_timeout_ms));
#else
        timeval accept_tv{};
        accept_tv.tv_sec = 0;
        accept_tv.tv_usec = 500 * 1000;
        (void)::setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &accept_tv, sizeof(accept_tv));
#endif

        upstream_ready.store(true);
        while (!upstream_stop.load()) {
            socket_t client = ::accept(listener, nullptr, nullptr);
            if (client == kInvalidSocket) {
#ifdef _WIN32
                const int err = yuan::platform::GetLastNativeError();
                if (yuan::platform::IsNativeRetryableError(err) ||
                    yuan::platform::ClassifyNativeError(err) == yuan::platform::NativeError::timed_out) {
                    continue;
                }
#else
                const int err = yuan::platform::GetLastNativeError();
                if (yuan::platform::IsNativeRetryableError(err)) {
                    continue;
                }
#endif
                continue;
            }

#ifdef _WIN32
            const DWORD client_recv_timeout_ms = 1500;
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char *>(&client_recv_timeout_ms),
                               sizeof(client_recv_timeout_ms));
#else
            timeval client_tv{};
            client_tv.tv_sec = 1;
            client_tv.tv_usec = 500 * 1000;
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof(client_tv));
#endif

            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
                const int rc = ::recv(client, buf, static_cast<int>(sizeof(buf)), 0);
#else
                const ssize_t rc = ::recv(client, buf, sizeof(buf), 0);
#endif
                if (rc <= 0) {
                    break;
                }
                req.append(buf, static_cast<std::size_t>(rc));
                if (req.size() > 16 * 1024) {
                    break;
                }
            }

            const auto req_line_end = req.find("\r\n");
            const std::string req_line = req_line_end == std::string::npos ? req : req.substr(0, req_line_end);
            {
                std::ofstream out(upstream_log_path, std::ios::binary | std::ios::app);
                out << req << "\n---\n";
            }

            const std::string body = req_line;
            std::string extra_response_headers;
            if (req_line.find("/proxy-cache/private") != std::string::npos) {
                extra_response_headers += "Cache-Control: private\r\n";
            }
            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "X-Upstream-Secret: hidden\r\n"
                "X-Replace-Me: upstream\r\n"
                "Location: http://upstream.local/internal/login\r\n" +
                extra_response_headers +
                "Connection: close\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
            (void)send_all(client, response);
            close_socket(client);
        }

        close_socket(listener);
    });

    for (int i = 0; i < 100 && !upstream_ready.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(upstream_ready.load(), "proxy upstream test server should start");

    nlohmann::json proxy_cfg;
    proxy_cfg["proxies"] = {
        {{"root", "/proxy-ok/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"strip_prefix", false},
         {"connect_timeout", 300},
         {"read_timeout", 1200},
         {"write_timeout", 800},
         {"max_retries", 1}},
        {{"root", "/proxy-fail/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", closed_proxy_port})})},
         {"strip_prefix", false},
         {"connect_timeout", 200},
         {"read_timeout", 1000},
         {"write_timeout", 500},
         {"max_retries", 0},
         {"failure_threshold", 1},
         {"unhealthy_cooldown_ms", 500}},
        {{"root", "/proxy-rewrite/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"strip_prefix", true},
         {"rewrite", "/rewritten"},
         {"connect_timeout", 300},
         {"read_timeout", 1200},
         {"write_timeout", 800},
         {"max_retries", 0}},
        {{"root", "/proxy-root/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"strip_prefix", true},
         {"connect_timeout", 300},
         {"read_timeout", 1200},
         {"write_timeout", 800},
         {"max_retries", 0}},
        {{"root", "/proxy-headers/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"strip_prefix", false},
         {"preserve_host", true},
         {"proxy_set_header", {{"X-Forwarded-Host", "$host"}, {"X-Request-Uri", "$request_uri"}, {"X-Proxy-Added", "$remote_addr"}}},
         {"hide_request_headers", nlohmann::json::array({"X-Remove-Me"})},
         {"proxy_hide_header", nlohmann::json::array({"X-Upstream-Secret"})},
         {"proxy_set_response_header", {{"X-Replace-Me", "edge"}, {"X-Proxy-Response", "yes"}}},
         {"allowed_methods", nlohmann::json::array({"GET"})},
         {"connect_timeout", 300},
         {"read_timeout", 1200},
         {"write_timeout", 800},
         {"max_retries", 0}},
        {{"root", "/proxy-redirect/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"strip_prefix", false},
         {"proxy_redirect", {{"http://upstream.local/internal", "/public"}}},
         {"connect_timeout", 300},
         {"read_timeout", 1200},
         {"write_timeout", 800},
         {"max_retries", 0}},
        {{"root", "/proxy-deny/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"deny", nlohmann::json::array({"127.0.0.1"})},
         {"max_retries", 0}},
        {{"root", "/proxy-exact"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"match", "exact"},
         {"strip_prefix", false},
         {"connect_timeout", 300},
         {"read_timeout", 1200},
         {"write_timeout", 800},
         {"max_retries", 0}},
        {{"root", "/proxy-priority/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"strip_prefix", false},
         {"proxy_set_response_header", {{"X-Priority-Route", "normal"}}},
         {"max_retries", 0}},
        {{"root", "^/proxy-priority/.*\\.json$"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"match", "~"},
         {"strip_prefix", false},
         {"proxy_set_response_header", {{"X-Priority-Route", "regex"}}},
         {"max_retries", 0}},
        {{"root", "/proxy-priority/strong/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"match", "^~"},
         {"strip_prefix", false},
         {"proxy_set_response_header", {{"X-Priority-Route", "strong"}}},
         {"max_retries", 0}},
        {{"root", "/proxy-cache/"},
         {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
         {"strip_prefix", false},
         {"proxy_cache", true},
         {"proxy_cache_valid", 60000},
         {"proxy_cache_max_size", 4096},
         {"proxy_cache_key", "$host$request_uri"},
         {"proxy_cache_bypass_headers", nlohmann::json::array({"X-Bypass-Cache"})},
         {"proxy_no_cache_headers", nlohmann::json::array({"X-No-Cache-Store"})},
         {"max_retries", 0}}};
    {
        std::ofstream out("http.json", std::ios::binary | std::ios::trunc);
        out << proxy_cfg.dump(2);
    }
    (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
    yuan::net::http::config::load_config();

    auto service = std::make_unique<yuan::server::HttpService>(port, cfg);
    if (!service->init()) {
        std::cerr << "http service init failed\n";
        return 1;
    }
    service->server().mount_static("/static", static_root.string());
    service->server().on("/__h2_large", [](yuan::net::http::HttpRequest *req,
                                           yuan::net::http::HttpResponse *resp) {
        const std::string body(60000, 'x');
        resp->set_response_code(yuan::net::http::ResponseCode::ok_);
        resp->add_header("Content-Type", "text/plain");
        resp->add_header("Content-Length", std::to_string(body.size()));
        resp->append_body(body);
        if (req->get_version() != yuan::net::http::HttpVersion::v_2_0) {
            resp->send();
        }
    });
    service->server().on("/__h2_window", [](yuan::net::http::HttpRequest *req,
                                            yuan::net::http::HttpResponse *resp) {
        const std::string body(70000, 'w');
        resp->set_response_code(yuan::net::http::ResponseCode::ok_);
        resp->add_header("Content-Type", "text/plain");
        resp->add_header("Content-Length", std::to_string(body.size()));
        resp->append_body(body);
        if (req->get_version() != yuan::net::http::HttpVersion::v_2_0) {
            resp->send();
        }
    });
    service->server().on("/__echo_body", [](yuan::net::http::HttpRequest *req,
                                            yuan::net::http::HttpResponse *resp) {
        std::string body;
        if (const char *begin = req->body_begin()) {
            if (const char *end = req->body_end()) {
                body.assign(begin, end);
            }
        }
        resp->set_response_code(yuan::net::http::ResponseCode::ok_);
        resp->add_header("Content-Type", "text/plain");
        resp->add_header("Content-Length", std::to_string(body.size()));
        resp->append_body(body);
        resp->send();
    });
    service->server().on("/__query_route", [](yuan::net::http::HttpRequest *req,
                                               yuan::net::http::HttpResponse *resp) {
        const std::string body = "task_id=" + std::to_string(req->get_param_int("task_id", 0)) +
                                 ";name=" + req->get_param("name") +
                                 ";space=" + req->get_param("space");
        resp->set_response_code(yuan::net::http::ResponseCode::ok_);
        resp->add_header("Content-Type", "text/plain");
        resp->add_header("Content-Length", std::to_string(body.size()));
        resp->append_body(body);
        resp->send();
    });
    service->server().on("/__auto_finish", [](yuan::net::http::HttpRequest *,
                                              yuan::net::http::HttpResponse *resp) {
        const std::string body = "auto-finish-ok";
        resp->set_response_code(yuan::net::http::ResponseCode::ok_);
        resp->add_header("Content-Type", "text/plain");
        resp->add_header("Content-Length", std::to_string(body.size()));
        resp->append_body(body);
    });
    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const std::string query_route_resp = http_get(port, "/__query_route?task_id=42&name=SmokeRole&space=hello+world");
    check(query_route_resp.find("200 OK") != std::string::npos,
          "registered route should match request path without query string");
    check(query_route_resp.find("task_id=42") != std::string::npos,
          "query parameters should remain available to route handlers");
    check(query_route_resp.find("name=SmokeRole") != std::string::npos,
          "second query parameter should remain available to route handlers");
    check(query_route_resp.find("space=hello world") != std::string::npos,
          "query parameters should decode plus as space");
    const std::string auto_finish_resp = http_get(port, "/__auto_finish");
    check(auto_finish_resp.find("200 OK") != std::string::npos,
          "handler without explicit send should auto-finish HTTP/1 response");
    check(auto_finish_resp.find("auto-finish-ok") != std::string::npos,
          "auto-finished HTTP/1 response should include handler body");

    test_http_caps_and_proxy_stats(port);
    test_http1_split_header_and_invalid_content_length(port);
    test_http1_chunked_request(port);
    test_http2_preface_gate(port);
    test_http2_tls_only_rejects_h2c();
    test_http2_preface_settings_ack(port);
    test_http2_fragmented_preface_settings_ack(port);
    test_http2_ping_ack(port);
    test_http2_invalid_window_update_goaway(port);
    test_http2_invalid_rst_stream_goaway(port);
    test_http2_continuation_without_headers_goaway(port);
    test_http2_headers_continuation_data_flow(port);
    test_http_version_gate(port);
    test_http_caps_with_config_flags(port);
    test_http2_minimal_response_echo(port);
    test_http2_minimal_response_caps(port);
    test_http2_large_response_data_split(port);
    test_http2_window_update_flushes_pending_data(port);
    test_http2_unknown_path_404(port);
    test_http2_configurable_dispatch_path(port);
    test_http2_invalid_pseudo_headers_400(port);
    test_http2_pseudo_after_regular_header_400(port);
    test_http2_hpack_indexed_headers_flow(port);

    test_proxy_unmatched_route_returns_404(port);
    test_proxy_connect_failure_returns_502(port);
    test_proxy_strip_prefix_rewrite(port);
    test_proxy_strip_prefix_empty_path(port);
    test_proxy_header_controls(port, upstream_log_path);
    test_proxy_redirect(port);
    test_proxy_method_limit(port);
    test_proxy_access_limit(port);
    test_proxy_exact_match(port);
    test_proxy_location_priority(port);
    test_proxy_cache(port, upstream_log_path);

    std::filesystem::remove("http.json", cleanup_ec);
    const nlohmann::json reset_cfg = {
        {"enable_http2", false},
        {"enable_http3", false}};
    {
        std::ofstream out("http.json", std::ios::binary);
        out << reset_cfg.dump(2);
    }
    (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
    yuan::net::http::config::load_config();
    const std::string reload_back = http_get(port, "/reload_config");
    check(reload_back.find("200") != std::string::npos, "reload_config should restore defaults before static tests");
    service->server().mount_static("/static", static_root.string());
    yuan::net::http::StaticMountOptions typed_options;
    typed_options.default_type = "application/x-default";
    typed_options.mime_types[".foo"] = "application/x-test";
    typed_options.gzip_min_length = 1;
    typed_options.gzip_comp_level = 6;
    typed_options.brotli_comp_level = 4;
    typed_options.gzip_vary = false;
    typed_options.gzip_types = {"application/x-test"};
    service->server().mount_static("/typed", static_root.string(), typed_options);
    yuan::net::http::StaticMountOptions gzip_policy_options;
    gzip_policy_options.gzip_min_length = 1;
    gzip_policy_options.gzip_types = {"text/plain"};
    gzip_policy_options.gzip_disable = {"BadBot"};
    gzip_policy_options.gzip_proxied = {"auth"};
    service->server().mount_static("/gzip-policy", static_root.string(), gzip_policy_options);
    yuan::net::http::StaticMountOptions readonly_options;
    readonly_options.allowed_methods = {"GET", "HEAD"};
    service->server().mount_static("/readonly", static_root.string(), readonly_options);
    yuan::net::http::StaticMountOptions blocked_options;
    blocked_options.access_rules.push_back(yuan::net::http::AccessRule{false, "all"});
    service->server().mount_static("/blocked", static_root.string(), blocked_options);
    yuan::net::http::StaticMountOptions exact_static_options;
    exact_static_options.exact_match = true;
    service->server().mount_static("/exact-static", static_root.string(), exact_static_options);
    yuan::net::http::StaticMountOptions normal_static_priority;
    normal_static_priority.headers.push_back({"X-Static-Route", "normal"});
    service->server().mount_static("/static-priority", (static_root / "normal-root").string(), normal_static_priority);
    yuan::net::http::StaticMountOptions regex_static_priority;
    regex_static_priority.match_type = yuan::net::http::StaticMountOptions::MatchType::regex;
    regex_static_priority.headers.push_back({"X-Static-Route", "regex"});
    service->server().mount_static("^/static-priority/.*\\.json$", static_root.string(), regex_static_priority);
    yuan::net::http::StaticMountOptions strong_static_priority;
    strong_static_priority.match_type = yuan::net::http::StaticMountOptions::MatchType::prefix_strong;
    strong_static_priority.headers.push_back({"X-Static-Route", "strong"});
    service->server().mount_static("/static-priority/strong", (static_root / "strong-root").string(), strong_static_priority);
    yuan::net::http::StaticMountOptions regex_only_static;
    regex_only_static.match_type = yuan::net::http::StaticMountOptions::MatchType::regex;
    regex_only_static.headers.push_back({"X-Static-Route", "regex-only"});
    service->server().mount_static("^/regex-only/.*\\.txt$", static_root.string(), regex_only_static);

    test_static_etag_and_304(port, static_root);
    test_static_mime_and_gzip_types(port, static_root);
    test_static_gzip_policy(port, static_root);
    test_static_method_limit(port, static_root);
    test_static_access_limit(port, static_root);
    test_static_exact_match(port);
    test_static_regex_location_priority(port);
    test_static_gzip(port, static_root);
    test_static_precompressed_br(port, static_root);
    test_static_stream_client_abort_releases_connection(port, static_root, *service);

    std::filesystem::remove("http.json", cleanup_ec);

    service->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    upstream_stop.store(true);
    if (socket_t wake = connect_loopback(upstream_port); wake != kInvalidSocket) {
        (void)send_all(wake,
                       "GET /__stop HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\n"
                       "Connection: close\r\n\r\n");
        close_socket(wake);
    }
    if (upstream_thread.joinable()) {
        upstream_thread.join();
    }
    std::filesystem::remove(upstream_log_path, cleanup_ec);

    const int exit_code = (g_failed > 0) ? 1 : 0;
    if (exit_code != 0) {
        std::cerr << "http feature tests failed=" << g_failed << '\n';
    } else {
        std::cout << "http feature tests passed\n";
    }
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    WSACleanup();
    ::ExitProcess(static_cast<UINT>(exit_code));
#endif
    return exit_code;
}
