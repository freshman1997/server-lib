#include "http_server.h"
#include "request.h"
#include "response.h"
#include "http_service.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

    void test_http_caps_and_proxy_stats(uint16_t port)
    {
        const std::string caps = http_get(port, "/__http_caps");
        check(caps.find("200") != std::string::npos, "__http_caps should return 200");
        check(caps.find("\"http1\":true") != std::string::npos, "__http_caps should report http1");
        check(caps.find("\"http2\":false") != std::string::npos, "__http_caps should default http2=false");
        check(caps.find("\"http3\":false") != std::string::npos, "__http_caps should default http3=false");

        const std::string stats = http_get(port, "/__proxy_stats");
        check(stats.find("200") != std::string::npos, "__proxy_stats should return 200");
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

        std::string headers_payload = ":method: POST\r\n:path: /h2-bridge\r\n";
        std::string headers;
        headers.resize(9 + headers_payload.size());
        headers[0] = static_cast<char>((headers_payload.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((headers_payload.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(headers_payload.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x00;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, headers_payload.data(), headers_payload.size());

        std::string cont_payload = ":authority: local.test\r\n";
        std::string cont;
        cont.resize(9 + cont_payload.size());
        cont[0] = static_cast<char>((cont_payload.size() >> 16) & 0xff);
        cont[1] = static_cast<char>((cont_payload.size() >> 8) & 0xff);
        cont[2] = static_cast<char>(cont_payload.size() & 0xff);
        cont[3] = 0x09;
        cont[4] = 0x04;
        cont[5] = 0x00;
        cont[6] = 0x00;
        cont[7] = 0x00;
        cont[8] = 0x01;
        std::memcpy(cont.data() + 9, cont_payload.data(), cont_payload.size());

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

        std::string headers_payload = ":method: POST\r\n:path: /__h2_echo\r\n:authority: local.test\r\n";
        std::string headers;
        headers.resize(9 + headers_payload.size());
        headers[0] = static_cast<char>((headers_payload.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((headers_payload.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(headers_payload.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x04;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, headers_payload.data(), headers_payload.size());

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

        std::string headers_payload = ":method: GET\r\n:path: /__http_caps\r\n:authority: local.test\r\n";
        std::string headers;
        headers.resize(9 + headers_payload.size());
        headers[0] = static_cast<char>((headers_payload.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((headers_payload.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(headers_payload.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x05;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, headers_payload.data(), headers_payload.size());

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

        std::string headers_payload = ":method: GET\r\n:path: /__h2_unknown_path\r\n:authority: local.test\r\n";
        std::string headers;
        headers.resize(9 + headers_payload.size());
        headers[0] = static_cast<char>((headers_payload.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((headers_payload.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(headers_payload.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x05;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, headers_payload.data(), headers_payload.size());

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
            const auto &headers_payload_resp = frames[idx_headers].second;
            check(headers_payload_resp.find(":status: 404") != std::string::npos ||
                  (!headers_payload_resp.empty() && static_cast<unsigned char>(headers_payload_resp[0]) == 0x8d),
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

        std::string headers_payload = ":method: GET\r\n:path: /__h2_extra\r\n:authority: local.test\r\n";
        std::string headers;
        headers.resize(9 + headers_payload.size());
        headers[0] = static_cast<char>((headers_payload.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((headers_payload.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(headers_payload.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x05;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, headers_payload.data(), headers_payload.size());

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
            const auto &headers_payload_resp = frames[idx_headers].second;
            check(headers_payload_resp.find(":status: 404") != std::string::npos ||
                  (!headers_payload_resp.empty() && static_cast<unsigned char>(headers_payload_resp[0]) == 0x8d),
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

        std::string headers_payload = ":method: GET\r\n:authority: local.test\r\n";
        std::string headers;
        headers.resize(9 + headers_payload.size());
        headers[0] = static_cast<char>((headers_payload.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((headers_payload.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(headers_payload.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x05;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, headers_payload.data(), headers_payload.size());

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
            const auto &hp = frames[idx_headers].second;
            check(hp.find(":status: 400") != std::string::npos ||
                  (!hp.empty() && static_cast<unsigned char>(hp[0]) == 0x8c),
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

        std::string headers_payload = "x-test: 1\r\n:method: GET\r\n:path: /__http_caps\r\n";
        std::string headers;
        headers.resize(9 + headers_payload.size());
        headers[0] = static_cast<char>((headers_payload.size() >> 16) & 0xff);
        headers[1] = static_cast<char>((headers_payload.size() >> 8) & 0xff);
        headers[2] = static_cast<char>(headers_payload.size() & 0xff);
        headers[3] = 0x01;
        headers[4] = 0x05;
        headers[5] = 0x00;
        headers[6] = 0x00;
        headers[7] = 0x00;
        headers[8] = 0x01;
        std::memcpy(headers.data() + 9, headers_payload.data(), headers_payload.size());

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
            const auto &hp = frames[idx_headers].second;
            check(hp.find(":status: 400") != std::string::npos ||
                  (!hp.empty() && static_cast<unsigned char>(hp[0]) == 0x8c),
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
            const auto &hp = frames[idx_headers].second;
            check(hp.find(":status: 404") != std::string::npos ||
                  (!hp.empty() && static_cast<unsigned char>(hp[0]) == 0x8d),
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
    (void)yuan::net::http::HttpConfigManager::get_instance()->reload_config();
    yuan::net::http::config::load_config();

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
    auto service = std::make_unique<yuan::server::HttpService>(port, cfg);
    if (!service->init()) {
        std::cerr << "http service init failed\n";
        return 1;
    }
    service->server().mount_static("/static", static_root.string());
    service->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    test_http_caps_and_proxy_stats(port);
    test_http2_preface_gate(port);
    test_http2_preface_settings_ack(port);
    test_http2_ping_ack(port);
    test_http2_invalid_window_update_goaway(port);
    test_http2_invalid_rst_stream_goaway(port);
    test_http2_continuation_without_headers_goaway(port);
    test_http2_headers_continuation_data_flow(port);
    test_http_version_gate(port);
    test_http_caps_with_config_flags(port);
    test_http2_minimal_response_echo(port);
    test_http2_minimal_response_caps(port);
    test_http2_unknown_path_404(port);
    test_http2_configurable_dispatch_path(port);
    test_http2_invalid_pseudo_headers_400(port);
    test_http2_pseudo_after_regular_header_400(port);
    test_http2_hpack_indexed_headers_flow(port);

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
