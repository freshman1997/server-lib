#include "http_server.h"
#include "http2/hpack_decoder.h"
#include "http2/hpack_encoder.h"
#include "request.h"
#include "response.h"
#include "http_service.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ops/config_manager.h"
#include "ops/option.h"
#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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

    socket_t connect_loopback(uint16_t port)
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
            "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
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
            0x82, 0x44, 0x86, 0x60, 0x75, 0x99, 0x84, 0x95,
            0x09, 0x87, 0x41, 0x8a, 0xa0, 0xe4, 0x1d, 0x13,
            0x9d, 0x09, 0xb8, 0xf0, 0x1e, 0x07,
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
            std::chrono::steady_clock::now() - begin).count();

        check(!resp.empty(), "proxy connect-failure response should not be empty");
        check(resp.find("502") != std::string::npos,
              "proxy connect failure should return 502");
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
            "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
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
            std::cerr << "[DEBUG] http/2.0 gate response:\n" << resp_h2 << "\n";
        }
        check(resp_h2.find("505") != std::string::npos,
              "HTTP/2.0 request should be rejected with 505 when protocol stack disabled");

        const std::string req_h3 =
            "GET /__http_caps HTTP/3.0\r\n"
            "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
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
            std::cerr << "[DEBUG] http/3.0 gate response:\n" << resp_h3 << "\n";
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
        hpack_append_indexed(hpack, 2);   // :method: GET
        hpack_append_indexed(hpack, 4);   // :path: /
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
            std::cerr << "[DEBUG] conditional response:\n" << second << "\n";
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
            std::cerr << "[DEBUG] gzip response:\n" << resp << "\n";
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
            std::cerr << "[DEBUG] precompressed br response:\n" << resp << "\n";
        }
        check(resp.find("content-encoding: br") != std::string::npos, "precompressed br should set br encoding");
        check(resp.find("fake-br-payload") != std::string::npos, "precompressed br should serve .br payload");
    }
}

int main()
{
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

    const uint16_t port = reserve_tcp_port();
    check(port != 0, "should reserve test server port");
    if (port == 0) {
        return 1;
    }

    const auto static_root = std::filesystem::current_path() / "test_http_static";
    std::error_code ec;
    std::filesystem::create_directories(static_root, ec);

    yuan::net::http::HttpServerConfig cfg;
    cfg.enable_keep_alive = false;

    const uint16_t upstream_port = reserve_tcp_port();
    check(upstream_port != 0, "should reserve proxy upstream test port");
    if (upstream_port == 0) {
        return 1;
    }

    const auto upstream_log_path = std::filesystem::current_path() / "mini_nginx_proxy_test_upstream.log";
    {
        std::ofstream clear_log(upstream_log_path, std::ios::binary | std::ios::trunc);
    }

    std::atomic_bool upstream_ready{ false };
    std::atomic_bool upstream_stop{ false };
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
                const int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK || err == WSAEINTR) {
                    continue;
                }
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
#endif
                continue;
            }

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
                out << req_line << '\n';
            }

            const std::string body = req_line;
            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
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
        {
            {"root", "/proxy-ok/"},
            {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
            {"strip_prefix", false},
            {"connect_timeout", 300},
            {"read_timeout", 1200},
            {"write_timeout", 800},
            {"max_retries", 1}
        },
        {
            {"root", "/proxy-fail/"},
            {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", 1})})},
            {"strip_prefix", false},
            {"connect_timeout", 200},
            {"read_timeout", 1000},
            {"write_timeout", 500},
            {"max_retries", 0},
            {"failure_threshold", 1},
            {"unhealthy_cooldown_ms", 500}
        },
        {
            {"root", "/proxy-rewrite/"},
            {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
            {"strip_prefix", true},
            {"rewrite", "/rewritten"},
            {"connect_timeout", 300},
            {"read_timeout", 1200},
            {"write_timeout", 800},
            {"max_retries", 0}
        },
        {
            {"root", "/proxy-root/"},
            {"target", nlohmann::json::array({nlohmann::json::array({"127.0.0.1", upstream_port})})},
            {"strip_prefix", true},
            {"connect_timeout", 300},
            {"read_timeout", 1200},
            {"write_timeout", 800},
            {"max_retries", 0}
        }
    };
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
    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    test_http_caps_and_proxy_stats(port);
    test_http2_preface_gate(port);
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

    std::filesystem::remove("http.json", cleanup_ec);
    const nlohmann::json reset_cfg = {
        {"enable_http2", false},
        {"enable_http3", false}
    };
    {
        std::ofstream out("http.json", std::ios::binary);
        out << reset_cfg.dump(2);
    }
    (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
    yuan::net::http::config::load_config();
    const std::string reload_back = http_get(port, "/reload_config");
    check(reload_back.find("200") != std::string::npos, "reload_config should restore defaults before static tests");
    service->server().mount_static("/static", static_root.string());

    test_static_etag_and_304(port, static_root);
    test_static_gzip(port, static_root);
    test_static_precompressed_br(port, static_root);

    std::filesystem::remove("http.json", cleanup_ec);

    service->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    upstream_stop.store(true);
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
