#include "server/context.h"
#include "server/ftp_server.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
#ifdef _WIN32
    using SocketHandle = SOCKET;
    constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
    using SocketHandle = int;
    constexpr SocketHandle kInvalidSocket = -1;
#endif

    void close_socket(SocketHandle fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void require(bool cond, const std::string &msg)
    {
        if (!cond) {
            throw std::runtime_error(msg);
        }
    }

    bool send_all(SocketHandle fd, const std::string &data)
    {
        size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            int n = send(fd, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
#endif
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool recv_line(SocketHandle fd, std::string &line)
    {
        line.clear();
        char ch = 0;
        while (line.size() < 8192) {
#ifdef _WIN32
            int n = recv(fd, &ch, 1, 0);
#else
            ssize_t n = recv(fd, &ch, 1, 0);
#endif
            if (n <= 0) {
                return false;
            }
            line.push_back(ch);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
                return true;
            }
        }
        return false;
    }

    int parse_code(const std::string &line)
    {
        if (line.size() < 3) {
            return 0;
        }
        return std::stoi(line.substr(0, 3));
    }

    SocketHandle connect_control(uint16_t port)
    {
        SocketHandle fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kInvalidSocket) {
            return kInvalidSocket;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

#ifdef _WIN32
        if (connect(fd, reinterpret_cast<const sockaddr *>(&addr), static_cast<int>(sizeof(addr))) != 0) {
#else
        if (connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
#endif
            close_socket(fd);
            return kInvalidSocket;
        }
        return fd;
    }

    SocketHandle create_data_listener(uint16_t &bound_port)
    {
        SocketHandle fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kInvalidSocket) {
            return kInvalidSocket;
        }

        int reuse = 1;
#ifdef _WIN32
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

#ifdef _WIN32
        if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), static_cast<int>(sizeof(addr))) != 0) {
#else
        if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
#endif
            close_socket(fd);
            return kInvalidSocket;
        }

        if (listen(fd, 1) != 0) {
            close_socket(fd);
            return kInvalidSocket;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int len = static_cast<int>(sizeof(bound));
#else
        socklen_t len = sizeof(bound);
#endif
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(fd);
            return kInvalidSocket;
        }
        bound_port = ntohs(bound.sin_port);
        return fd;
    }

    SocketHandle accept_data(SocketHandle listener)
    {
        sockaddr_in addr{};
#ifdef _WIN32
        int len = static_cast<int>(sizeof(addr));
#else
        socklen_t len = sizeof(addr);
#endif
        return accept(listener, reinterpret_cast<sockaddr *>(&addr), &len);
    }

    std::string recv_all(SocketHandle fd)
    {
        std::string out;
        char buf[4096];
        while (true) {
#ifdef _WIN32
            int n = recv(fd, buf, static_cast<int>(sizeof(buf)), 0);
#else
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                break;
            }
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    std::string make_port_cmd(uint16_t port)
    {
        const int p1 = port / 256;
        const int p2 = port % 256;
        return "PORT 127,0,0,1," + std::to_string(p1) + "," + std::to_string(p2) + "\r\n";
    }
}

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
#endif

    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "ftp_active_e2e_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "sample.txt") << "active-mode-sample";

    auto ctx = yuan::net::ftp::ServerContext::get_instance();
    ctx->set_server_work_dir(root.generic_string());
    ctx->set_auth_credential("tester", "secret");

    yuan::net::ftp::FtpServer server;
    std::thread server_thread([&]() {
        server.serve(12124);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    SocketHandle ctrl = connect_control(12124);
    require(ctrl != kInvalidSocket, "failed to connect control channel");

    std::string line;
    require(recv_line(ctrl, line), "failed to read welcome");
    require(parse_code(line) == 220, "welcome code should be 220");

    require(send_all(ctrl, "USER tester\r\n"), "failed to send USER");
    require(recv_line(ctrl, line), "failed to read USER response");
    require(parse_code(line) == 331, "USER response should be 331");

    require(send_all(ctrl, "PASS secret\r\n"), "failed to send PASS");
    require(recv_line(ctrl, line), "failed to read PASS response");
    require(parse_code(line) == 230, "PASS response should be 230");

    uint16_t list_port = 0;
    SocketHandle list_listener = create_data_listener(list_port);
    require(list_listener != kInvalidSocket, "failed to create LIST data listener");
    require(send_all(ctrl, make_port_cmd(list_port)), "failed to send PORT for LIST");
    require(recv_line(ctrl, line), "failed to read PORT LIST response");
    require(parse_code(line) == 200, "PORT response should be 200");
    require(send_all(ctrl, "LIST\r\n"), "failed to send LIST");
    require(recv_line(ctrl, line), "failed to read LIST pre response");
    int code = parse_code(line);
    require(code == 150 || code == 125, "LIST pre response should be 150/125");
    SocketHandle list_data = accept_data(list_listener);
    require(list_data != kInvalidSocket, "failed to accept LIST data connection");
    std::string list_payload = recv_all(list_data);
    close_socket(list_data);
    close_socket(list_listener);
    require(recv_line(ctrl, line), "failed to read LIST final response");
    require(parse_code(line) == 226, "LIST final response should be 226");
    require(list_payload.find("sample.txt") != std::string::npos, "LIST payload missing sample.txt");

    uint16_t retr_port = 0;
    SocketHandle retr_listener = create_data_listener(retr_port);
    require(retr_listener != kInvalidSocket, "failed to create RETR data listener");
    require(send_all(ctrl, make_port_cmd(retr_port)), "failed to send PORT for RETR");
    require(recv_line(ctrl, line), "failed to read PORT RETR response");
    require(parse_code(line) == 200, "PORT response should be 200");
    require(send_all(ctrl, "RETR sample.txt\r\n"), "failed to send RETR");
    require(recv_line(ctrl, line), "failed to read RETR pre response");
    code = parse_code(line);
    require(code == 150 || code == 125, "RETR pre response should be 150/125");
    SocketHandle retr_data = accept_data(retr_listener);
    require(retr_data != kInvalidSocket, "failed to accept RETR data connection");
    std::string retr_payload = recv_all(retr_data);
    close_socket(retr_data);
    close_socket(retr_listener);
    require(recv_line(ctrl, line), "failed to read RETR final response");
    require(parse_code(line) == 226, "RETR final response should be 226");
    require(retr_payload == "active-mode-sample", "RETR payload mismatch");

    uint16_t stor_port = 0;
    SocketHandle stor_listener = create_data_listener(stor_port);
    require(stor_listener != kInvalidSocket, "failed to create STOR data listener");
    require(send_all(ctrl, make_port_cmd(stor_port)), "failed to send PORT for STOR");
    require(recv_line(ctrl, line), "failed to read PORT STOR response");
    require(parse_code(line) == 200, "PORT response should be 200");
    require(send_all(ctrl, "STOR uploaded.txt\r\n"), "failed to send STOR");
    require(recv_line(ctrl, line), "failed to read STOR pre response");
    code = parse_code(line);
    require(code == 150 || code == 125, "STOR pre response should be 150/125");
    SocketHandle stor_data = accept_data(stor_listener);
    require(stor_data != kInvalidSocket, "failed to accept STOR data connection");
    require(send_all(stor_data, "active-upload"), "failed to send STOR payload");
#ifdef _WIN32
    shutdown(stor_data, SD_SEND);
#else
    shutdown(stor_data, SHUT_WR);
#endif
    close_socket(stor_data);
    close_socket(stor_listener);
    require(recv_line(ctrl, line), "failed to read STOR final response");
    require(parse_code(line) == 226, "STOR final response should be 226");

    std::ifstream uploaded(root / "uploaded.txt", std::ios::binary);
    std::string uploaded_content((std::istreambuf_iterator<char>(uploaded)), std::istreambuf_iterator<char>());
    require(uploaded_content == "active-upload", "uploaded.txt content mismatch");

    (void)send_all(ctrl, "QUIT\r\n");
    (void)recv_line(ctrl, line);
    close_socket(ctrl);

    server.quit();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    ctx->clear_auth_credential();
    fs::remove_all(root, ec);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
