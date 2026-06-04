#include "algorithm/ssh_algorithm_registry.h"
#include "algorithm/ssh_host_key_algorithm.h"
#include "auth/ssh_auth_publickey.h"
#include "crypto/ssh_crypto_openssl.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_config.h"
#include "transport/ssh_transport.h"
#include "transport/ssh_version_exchange.h"
#include "base/utils/base64.h"
#include "platform/native_platform.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <cerrno>
#include <unordered_map>
#include <vector>

#include "openssl/bn.h"
#include "openssl/core_names.h"
#include "openssl/evp.h"
#include "openssl/pem.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
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

    struct SocketGuard
    {
        SocketHandle fd = kInvalidSocket;

        ~SocketGuard()
        {
            close();
        }

        void close()
        {
            if (fd == kInvalidSocket) {
                return;
            }
#ifdef _WIN32
            closesocket(fd);
#else
            ::close(fd);
#endif
            fd = kInvalidSocket;
        }
    };

#ifndef _WIN32
    volatile sig_atomic_t g_cli_sigint_count = 0;

    void cli_sigint_handler(int)
    {
        g_cli_sigint_count = 1;
    }
#endif

    struct CliArgs
    {
        enum class HostKeyPolicy {
            no,
            accept_new,
            yes
        };

        std::string host;
        int port = 22;
        int timeout_ms = 5000;
        std::string user;
        std::string password;
        std::string command;
        std::string config_file;
        std::vector<std::string> local_forwards;
        std::vector<std::string> dynamic_forwards;
        std::vector<std::string> remote_forwards;
        std::vector<std::string> identity_files;
        std::map<std::string, std::string> options;
        bool batch_mode = false;
        bool quiet = false;
        int verbose = 0;
        bool help = false;
        bool version = false;
        bool probe = false;
        bool stderr_prefix = false;
        HostKeyPolicy host_key_policy = HostKeyPolicy::no;
        std::string known_hosts_file;
    };

    int read_env_int(const char *name, int default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0') {
            return default_value;
        }
        try {
            return std::stoi(raw);
        } catch (...) {
            return default_value;
        }
    }

    std::string read_env_string(const char *name, const std::string &default_value = {})
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

    std::string to_lower_ascii(std::string value)
    {
        for (char &ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    bool parse_host_key_policy_value(const std::string & value,
                                     CliArgs::HostKeyPolicy & policy_out)
    {
        const auto value_lc = to_lower_ascii(value);
        if (value_lc == "yes" || value_lc == "true" || value_lc == "on") {
            policy_out = CliArgs::HostKeyPolicy::yes;
            return true;
        }
        if (value_lc == "accept-new") {
            policy_out = CliArgs::HostKeyPolicy::accept_new;
            return true;
        }
        if (value_lc == "no" || value_lc == "false" || value_lc == "off") {
            policy_out = CliArgs::HostKeyPolicy::no;
            return true;
        }
        return false;
    }

    std::string trim_copy(const std::string & value)
    {
        size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
            ++begin;
        }
        size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    std::vector<std::string> split_ws(const std::string & line)
    {
        std::vector<std::string> parts;
        std::istringstream iss(line);
        std::string token;
        while (iss >> token) {
            parts.push_back(std::move(token));
        }
        return parts;
    }

    bool host_matches_known_hosts_field(const std::string & host_field,
                                        const std::string & expected_host_token,
                                        const std::string & expected_host_plain)
    {
        size_t start = 0;
        while (start <= host_field.size()) {
            const size_t comma = host_field.find(',', start);
            const size_t end = comma == std::string::npos ? host_field.size() : comma;
            const std::string candidate = host_field.substr(start, end - start);
            if (candidate == expected_host_token || candidate == expected_host_plain) {
                return true;
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        return false;
    }

    std::string default_known_hosts_file()
    {
        const std::string home = read_env_string("HOME");
        if (!home.empty()) {
            return (std::filesystem::path(home) / ".ssh" / "known_hosts").string();
        }
#ifdef _WIN32
        const std::string user_profile = read_env_string("USERPROFILE");
        if (!user_profile.empty()) {
            return (std::filesystem::path(user_profile) / ".ssh" / "known_hosts").string();
        }
        const std::string home_drive = read_env_string("HOMEDRIVE");
        const std::string home_path = read_env_string("HOMEPATH");
        if (!home_drive.empty() && !home_path.empty()) {
            return (std::filesystem::path(home_drive + home_path) / ".ssh" / "known_hosts").string();
        }
#endif
        return "known_hosts";
    }

    void print_usage(const char *program)
    {
        std::cout
            << "release_ssh_cli\n"
            << "usage:\n"
            << "  " << program << " [options] [user@]host [command]\n"
            << "  " << program << " --probe -p 2222 yuan@127.0.0.1\n\n"
            << "options:\n"
            << "  -p <port>                 Port to connect to\n"
            << "  -l <login_name>           Login username\n"
            << "  -i <identity_file>        Private key file for publickey auth\n"
            << "  -L <[bind_addr:]port:host:hostport> Local forward rule\n"
            << "  -D <[bind_addr:]port>     Dynamic SOCKS5 forward\n"
            << "  -R <[bind_addr:]port:host:hostport> Remote forward rule\n"
            << "  -o <key=value>            OpenSSH-style option override\n"
            << "  -F <config_file>          Record config file option\n"
            << "      --known-hosts <path>  Known hosts path (default ~/.ssh/known_hosts)\n"
            << "      --strict-host-key-checking <yes|accept-new|no> Host key policy\n"
            << "  -q                        Quiet mode\n"
            << "  -v                        Verbose mode, repeatable\n"
            << "  -V, --version             Print version\n"
            << "  -h, --help                Show this help\n"
            << "      --host <host>         Explicit host, for script compatibility\n"
            << "      --port <port>         Explicit port, for script compatibility\n"
            << "      --user <user>         Explicit user, for script compatibility\n"
            << "      --password <password> Password auth secret (or YUAN_SSH_PASSWORD)\n"
            << "      --command <cmd>       Command to execute\n"
            << "      --timeout-ms <ms>     Socket receive timeout\n"
            << "      --stderr-prefix       Prefix stderr lines with [stderr]\\n"
            << "      --probe               Only verify TCP + SSH version exchange\n\n"
            << "env defaults:\n"
            << "  YUAN_SSH_HOST\n"
            << "  YUAN_SSH_PORT\n"
            << "  YUAN_SSH_TIMEOUT_MS\n"
            << "  YUAN_SSH_USER\n"
            << "  YUAN_SSH_PASSWORD\n"
            << "  YUAN_SSH_COMMAND\n";
    }

    bool parse_int_value(const std::string &raw, int &out)
    {
        try {
            size_t pos = 0;
            const int value = std::stoi(raw, &pos);
            if (pos != raw.size()) {
                return false;
            }
            out = value;
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string join_args(int argc, char **argv, int start)
    {
        std::ostringstream out;
        for (int i = start; i < argc; ++i) {
            if (i > start) {
                out << ' ';
            }
            out << argv[i];
        }
        return out.str();
    }

    void apply_option(CliArgs &args, const std::string &raw)
    {
        const auto eq = raw.find('=');
        const std::string key = eq == std::string::npos ? raw : raw.substr(0, eq);
        const std::string value = eq == std::string::npos ? "yes" : raw.substr(eq + 1);
        args.options[key] = value;

        const std::string key_lc = to_lower_ascii(key);

        if (key_lc == "port") {
            int port = 0;
            if (parse_int_value(value, port)) {
                args.port = port;
            }
        } else if (key_lc == "user") {
            args.user = value;
        } else if (key_lc == "batchmode") {
            args.batch_mode = (value == "yes" || value == "true" || value == "1");
        } else if (key_lc == "connecttimeout") {
            int seconds = 0;
            if (parse_int_value(value, seconds) && seconds > 0) {
                args.timeout_ms = seconds * 1000;
            }
        } else if (key_lc == "stricthostkeychecking") {
            (void)parse_host_key_policy_value(value, args.host_key_policy);
        } else if (key_lc == "userknownhostsfile") {
            args.known_hosts_file = value;
        }
    }

    void parse_destination(const std::string &destination, CliArgs &args)
    {
        const auto at = destination.find('@');
        if (at != std::string::npos) {
            args.user = destination.substr(0, at);
            args.host = destination.substr(at + 1);
        } else {
            args.host = destination;
        }
    }

    bool parse_args(int argc, char **argv, CliArgs &args)
    {
        args.host = read_env_string("YUAN_SSH_HOST", args.host);
        args.port = read_env_int("YUAN_SSH_PORT", args.port);
        args.timeout_ms = read_env_int("YUAN_SSH_TIMEOUT_MS", args.timeout_ms);
        args.user = read_env_string("YUAN_SSH_USER", args.user);
        args.password = read_env_string("YUAN_SSH_PASSWORD", args.password);
        args.command = read_env_string("YUAN_SSH_COMMAND", args.command);

        int idx = 1;
        while (idx < argc) {
            const std::string opt = argv[idx++];
            if (opt == "-h" || opt == "--help") {
                args.help = true;
                return true;
            }
            if (opt == "-V" || opt == "--version") {
                args.version = true;
                return true;
            }
            if (opt == "-q") {
                args.quiet = true;
                continue;
            }
            if (opt == "-v") {
                ++args.verbose;
                continue;
            }
            if (opt == "--probe") {
                args.probe = true;
                continue;
            }
            if (opt == "--stderr-prefix") {
                args.stderr_prefix = true;
                continue;
            }
            if (opt == "--") {
                if (idx < argc && args.host.empty()) {
                    parse_destination(argv[idx++], args);
                }
                if (idx < argc) {
                    args.command = join_args(argc, argv, idx);
                }
                break;
            }
            if (!opt.empty() && opt[0] != '-') {
                parse_destination(opt, args);
                if (idx < argc) {
                    args.command = join_args(argc, argv, idx);
                }
                break;
            }

            if (idx >= argc) {
                std::cerr << "missing value for " << opt << '\n';
                return false;
            }
            const std::string value = argv[idx++];

            if (opt == "--host") {
                args.host = value;
            } else if (opt == "-p" || opt == "--port") {
                if (!parse_int_value(value, args.port)) {
                    std::cerr << "invalid port: " << value << '\n';
                    return false;
                }
            } else if (opt == "--timeout-ms") {
                if (!parse_int_value(value, args.timeout_ms)) {
                    std::cerr << "invalid timeout: " << value << '\n';
                    return false;
                }
            } else if (opt == "-l" || opt == "--user") {
                args.user = value;
            } else if (opt == "--password") {
                args.password = value;
            } else if (opt == "--command") {
                args.command = value;
            } else if (opt == "--known-hosts") {
                args.known_hosts_file = value;
            } else if (opt == "--strict-host-key-checking") {
                if (!parse_host_key_policy_value(value, args.host_key_policy)) {
                    std::cerr << "invalid strict host key checking mode: " << value << '\n';
                    return false;
                }
            } else if (opt == "-i") {
                args.identity_files.push_back(value);
            } else if (opt == "-L") {
                args.local_forwards.push_back(value);
            } else if (opt == "-D") {
                args.dynamic_forwards.push_back(value);
            } else if (opt == "-R") {
                args.remote_forwards.push_back(value);
            } else if (opt == "-o") {
                apply_option(args, value);
            } else if (opt == "-F") {
                args.config_file = value;
            } else {
                std::cerr << "unknown option: " << opt << '\n';
                return false;
            }
        }

        if (args.host.empty() || args.port <= 0 || args.port > 65535) {
            std::cerr << "invalid host/port\n";
            return false;
        }
        if (args.timeout_ms < 100) {
            args.timeout_ms = 100;
        }
        if (args.probe) {
            return true;
        }
        if (args.user.empty()) {
            std::cerr << "--user is required\n";
            return false;
        }
        if (!args.identity_files.empty() && args.identity_files.size() > 1) {
            std::cerr << "multiple -i identity files are not supported yet\n";
            return false;
        }
        if (args.password.empty() && args.identity_files.empty()) {
            std::cerr << "either --password (or YUAN_SSH_PASSWORD) or -i <identity_file> is required\n";
            return false;
        }
        return true;
    }

    bool set_recv_timeout(SocketHandle fd, int timeout_ms)
    {
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(timeout_ms);
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0;
#else
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
    }

    bool connect_tcp(const std::string &host, uint16_t port, SocketGuard &sock)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
            return false;
        }

        bool ok = false;
        for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
            SocketHandle fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd == kInvalidSocket) {
                continue;
            }

#ifdef _WIN32
            const int rc = ::connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen));
#else
            const int rc = ::connect(fd, it->ai_addr, it->ai_addrlen);
#endif
            if (rc == 0) {
                sock.close();
                sock.fd = fd;
                ok = true;
                break;
            }

#ifdef _WIN32
            closesocket(fd);
#else
            ::close(fd);
#endif
        }

        freeaddrinfo(result);
        return ok;
    }

    bool socket_read_ready(SocketHandle fd)
    {
        if (fd == kInvalidSocket) {
            return false;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeval tv{};
#ifdef _WIN32
        const int rc = select(0, &readfds, nullptr, nullptr, &tv);
#else
        const int rc = select(fd + 1, &readfds, nullptr, nullptr, &tv);
#endif
        return rc > 0 && FD_ISSET(fd, &readfds);
    }

    bool socket_would_block_last_error()
    {
        const int err = yuan::platform::GetLastNativeError();
        if (yuan::platform::ClassifyNativeError(err) == yuan::platform::NativeError::timed_out) {
            return true;
        }
        return yuan::platform::IsNativeRetryableError(err);
    }

    void shutdown_socket_write(SocketHandle fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        (void)shutdown(fd, SD_SEND);
#else
        (void)shutdown(fd, SHUT_WR);
#endif
    }

    void close_socket_handle(SocketHandle &fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
        fd = kInvalidSocket;
    }

    bool set_socket_nonblocking(SocketHandle fd)
    {
        if (fd == kInvalidSocket) {
            return false;
        }
#ifdef _WIN32
        u_long mode = 1;
        return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            return false;
        }
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    bool create_listen_socket(const std::string & bind_addr,
                              uint16_t bind_port,
                              SocketHandle & out_fd)
    {
        out_fd = kInvalidSocket;

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo * result = nullptr;
        const std::string service = std::to_string(bind_port);
        const char * host = bind_addr.empty() ? nullptr : bind_addr.c_str();
        if (getaddrinfo(host, service.c_str(), &hints, &result) != 0) {
            return false;
        }

        bool ok = false;
        for (addrinfo * it = result; it != nullptr; it = it->ai_next) {
            SocketHandle fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd == kInvalidSocket) {
                continue;
            }

            int reuse = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                             reinterpret_cast<const char *>(&reuse),
                             sizeof(reuse));

#ifdef _WIN32
            if (::bind(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) != 0) {
                closesocket(fd);
                continue;
            }
#else
            if (::bind(fd, it->ai_addr, it->ai_addrlen) != 0) {
                ::close(fd);
                continue;
            }
#endif

            if (listen(fd, 128) != 0 || !set_socket_nonblocking(fd)) {
#ifdef _WIN32
                closesocket(fd);
#else
                ::close(fd);
#endif
                continue;
            }

            out_fd = fd;
            ok = true;
            break;
        }

        freeaddrinfo(result);
        return ok;
    }

    bool accept_pending_client(SocketHandle listen_fd,
                               SocketHandle & accepted_fd,
                               std::string & origin_host,
                               uint16_t & origin_port,
                               bool & had_fatal_error)
    {
        accepted_fd = kInvalidSocket;
        origin_host = "127.0.0.1";
        origin_port = 0;
        had_fatal_error = false;

        sockaddr_storage addr{};
#ifdef _WIN32
        int addr_len = static_cast<int>(sizeof(addr));
#else
        socklen_t addr_len = sizeof(addr);
#endif
        SocketHandle fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&addr), &addr_len);
        if (fd == kInvalidSocket) {
            if (socket_would_block_last_error()) {
                return false;
            }
            had_fatal_error = true;
            return false;
        }

        if (!set_socket_nonblocking(fd)) {
            close_socket_handle(fd);
            had_fatal_error = true;
            return false;
        }

        char host_buf[NI_MAXHOST] = {};
        char serv_buf[NI_MAXSERV] = {};
        if (getnameinfo(reinterpret_cast<const sockaddr *>(&addr),
                        addr_len,
                        host_buf,
                        sizeof(host_buf),
                        serv_buf,
                        sizeof(serv_buf),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            origin_host = host_buf;
            char * end = nullptr;
            unsigned long parsed = std::strtoul(serv_buf, &end, 10);
            if (end != serv_buf && *end == '\0' && parsed <= 65535ul) {
                origin_port = static_cast<uint16_t>(parsed);
            }
        }

        accepted_fd = fd;
        return true;
    }

    bool send_all(SocketHandle fd, const uint8_t *data, size_t len)
    {
        size_t sent = 0;
        while (sent < len) {
#ifdef _WIN32
            const int n = send(fd, reinterpret_cast<const char *>(data + sent), static_cast<int>(len - sent), 0);
#else
            const ssize_t n = send(fd, data + sent, len - sent, 0);
#endif
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    enum class RecvStatus {
        data,
        timeout,
        closed_or_error
    };

    RecvStatus recv_some(SocketHandle fd, std::vector<uint8_t> &out)
    {
        std::array<uint8_t, 64 * 1024> buf{};
#ifdef _WIN32
        const int n = recv(fd, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0);
#else
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            const int err = yuan::platform::GetLastNativeError();
            if (yuan::platform::IsNativeRetryableError(err) ||
                yuan::platform::ClassifyNativeError(err) == yuan::platform::NativeError::timed_out) {
                return RecvStatus::timeout;
            }
#else
            const int err = yuan::platform::GetLastNativeError();
            if (yuan::platform::IsNativeRetryableError(err)) {
                return RecvStatus::timeout;
            }
#endif
            return RecvStatus::closed_or_error;
        }
        out.assign(buf.begin(), buf.begin() + n);
        return RecvStatus::data;
    }

    bool recv_line(SocketHandle fd, std::string &line)
    {
        line.clear();
        std::array<char, 1> byte{};
        while (line.size() < 1024) {
#ifdef _WIN32
            const int n = recv(fd, byte.data(), 1, 0);
#else
            const ssize_t n = recv(fd, byte.data(), 1, 0);
#endif
            if (n <= 0) {
                return false;
            }
            line.push_back(byte[0]);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
                return true;
            }
        }
        return false;
    }

    bool stdin_has_data_nonblocking()
    {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            return false;
        }

        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        }

        if (GetFileType(h) != FILE_TYPE_PIPE) {
            return false;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        return available > 0;
#else
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        const int rc = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv);
        return rc > 0 && FD_ISSET(STDIN_FILENO, &readfds);
#endif
    }

    enum class StdinPollResult {
        no_data,
        data,
        eof,
        error
    };

    StdinPollResult read_stdin_chunk_nonblocking(std::string & out)
    {
        out.clear();
#ifdef _WIN32
        if (!stdin_has_data_nonblocking()) {
            return StdinPollResult::no_data;
        }

        std::array<char, 4096> buf{};
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            return StdinPollResult::error;
        }

        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            DWORD n_chars = 0;
            if (!ReadConsoleA(h, buf.data(), static_cast<DWORD>(buf.size()), &n_chars, nullptr)) {
                return StdinPollResult::error;
            }
            if (n_chars == 0) {
                return StdinPollResult::eof;
            }
            out.assign(buf.data(), static_cast<size_t>(n_chars));
            return StdinPollResult::data;
        }

        DWORD n = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                return StdinPollResult::eof;
            }
            if (err == ERROR_NO_DATA) {
                return StdinPollResult::no_data;
            }
            return StdinPollResult::error;
        }
        if (n == 0) {
            return StdinPollResult::eof;
        }
        out.assign(buf.data(), static_cast<size_t>(n));
        return StdinPollResult::data;
#else
        if (!stdin_has_data_nonblocking()) {
            return StdinPollResult::no_data;
        }

        std::array<char, 4096> buf{};
        const ssize_t n = ::read(STDIN_FILENO, buf.data(), buf.size());
        if (n == 0) {
            return StdinPollResult::eof;
        }
        if (n < 0) {
            const int err = yuan::platform::GetLastNativeError();
            if (yuan::platform::IsNativeRetryableError(err)) {
                return StdinPollResult::no_data;
            }
            return StdinPollResult::error;
        }
        out.assign(buf.data(), static_cast<size_t>(n));
        return StdinPollResult::data;
#endif
    }

    bool run_version_probe(SocketHandle fd, const CliArgs &args)
    {
        const std::string client_version = "SSH-2.0-YuanReleaseCliProbe_0.1\r\n";
        if (!send_all(fd,
                      reinterpret_cast<const uint8_t *>(client_version.data()),
                      client_version.size())) {
            std::cerr << "failed to send client version\n";
            return false;
        }

        std::string server_version_line;
        if (!recv_line(fd, server_version_line)) {
            std::cerr << "failed to read server version\n";
            return false;
        }
        auto server_info = yuan::net::ssh::SshVersionExchange::parse_version_line(server_version_line);
        if (!server_info || !yuan::net::ssh::SshVersionExchange::is_valid_protocol_version(server_info->protocol_version)) {
            std::cerr << "invalid server version line\n";
            return false;
        }
        if (!args.quiet) {
            std::cout << server_info->raw_line << '\n';
        }
        return true;
    }

    enum class PacketReadStatus {
        ok,
        timeout,
        eof_or_error
    };

    PacketReadStatus read_packet(SocketHandle fd,
                                 yuan::buffer::ByteBuffer &recv_buf,
                                 yuan::net::ssh::SshTransport &transport,
                                 std::vector<uint8_t> &payload)
    {
        for (;;) {
            auto parse = transport.try_parse_packet(recv_buf);
            if (parse.invalid) {
                return PacketReadStatus::eof_or_error;
            }
            if (!parse.complete) {
                std::vector<uint8_t> chunk;
                const auto recv_status = recv_some(fd, chunk);
                if (recv_status == RecvStatus::timeout) {
                    return PacketReadStatus::timeout;
                }
                if (recv_status != RecvStatus::data) {
                    return PacketReadStatus::eof_or_error;
                }
                recv_buf.append(chunk.data(), chunk.size());
                continue;
            }

            auto decoded = transport.decode_packet(
                reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()),
                parse.total_bytes);
            transport.increment_recv_seq();
            recv_buf.consume(parse.total_bytes);

            if (!decoded) {
                return PacketReadStatus::eof_or_error;
            }
            payload = std::move(*decoded);
            return PacketReadStatus::ok;
        }
    }

    bool send_packet(SocketHandle fd,
                     yuan::net::ssh::SshTransport &transport,
                     const yuan::buffer::ByteBuffer &payload)
    {
        auto packet = transport.encode_packet(
            reinterpret_cast<const uint8_t *>(payload.read_ptr()),
            payload.readable_bytes());
        transport.increment_send_seq();
        return send_all(fd,
                        reinterpret_cast<const uint8_t *>(packet.read_ptr()),
                        packet.readable_bytes());
    }

    bool check_known_hosts(const CliArgs & args,
                           const std::vector<uint8_t> & host_key_blob,
                           std::string & error_message)
    {
        size_t offset = 0;
        auto host_key_algorithm = yuan::net::ssh::SshMessageCodec::read_string(
            host_key_blob.data(), host_key_blob.size(), offset);
        if (!host_key_algorithm) {
            error_message = "invalid server host key blob";
            return false;
        }

        const std::string host_key_b64 = yuan::base::util::base64_encode(host_key_blob);
        const std::string host_token =
            args.port == 22 ? args.host : ("[" + args.host + "]:" + std::to_string(args.port));
        const std::string host_plain = args.host;

        const std::filesystem::path known_hosts_path =
            args.known_hosts_file.empty() ? std::filesystem::path(default_known_hosts_file())
                                          : std::filesystem::path(args.known_hosts_file);

        bool found_exact = false;
        bool found_conflict = false;
        std::ifstream in(known_hosts_path);
        std::string line;
        while (std::getline(in, line)) {
            const std::string trimmed = trim_copy(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            const auto parts = split_ws(trimmed);
            if (parts.size() < 3) {
                continue;
            }

            size_t host_index = 0;
            size_t algo_index = 1;
            size_t key_index = 2;
            if (!parts.empty() && !parts[0].empty() && parts[0][0] == '@') {
                if (parts.size() < 4) {
                    continue;
                }
                host_index = 1;
                algo_index = 2;
                key_index = 3;
            }

            const std::string & hosts_field = parts[host_index];
            if (!hosts_field.empty() && hosts_field[0] == '|') {
                continue;
            }

            if (!host_matches_known_hosts_field(hosts_field, host_token, host_plain)) {
                continue;
            }

            if (parts[algo_index] == *host_key_algorithm && parts[key_index] == host_key_b64) {
                found_exact = true;
                break;
            }

            found_conflict = true;
        }

        if (found_conflict && args.host_key_policy != CliArgs::HostKeyPolicy::no) {
            error_message = "host key mismatch for " + host_token;
            return false;
        }

        if (found_exact) {
            return true;
        }

        if (args.host_key_policy == CliArgs::HostKeyPolicy::yes) {
            error_message = "host key not found in known_hosts for " + host_token;
            return false;
        }

        std::error_code ec;
        const auto parent = known_hosts_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream out(known_hosts_path, std::ios::app);
        if (!out.good()) {
            error_message = "failed to update known_hosts file: " + known_hosts_path.string();
            return false;
        }
        out << host_token << ' ' << *host_key_algorithm << ' ' << host_key_b64 << '\n';
        return true;
    }

    std::vector<std::string> split_name_list(const std::string &csv)
    {
        std::vector<std::string> names;
        size_t start = 0;
        while (start <= csv.size()) {
            const size_t comma = csv.find(',', start);
            if (comma == std::string::npos) {
                if (start < csv.size()) {
                    names.push_back(csv.substr(start));
                }
                break;
            }
            if (comma > start) {
                names.push_back(csv.substr(start, comma - start));
            }
            start = comma + 1;
        }
        return names;
    }

    bool list_contains(const std::vector<std::string> &names, std::string_view needle)
    {
        for (const auto &name : names) {
            if (name == needle) {
                return true;
            }
        }
        return false;
    }

    std::optional<std::vector<uint8_t> > load_file_bytes(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            return std::nullopt;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return data;
    }

    std::vector<uint8_t> make_password_method_data(const std::string &password)
    {
        yuan::buffer::ByteBuffer method;
        yuan::net::ssh::SshMessageCodec::write_boolean(method, false);
        yuan::net::ssh::SshMessageCodec::write_string(method, password);
        return {
            reinterpret_cast<const uint8_t *>(method.read_ptr()),
            reinterpret_cast<const uint8_t *>(method.read_ptr()) + method.readable_bytes()
        };
    }

    std::vector<uint8_t> buffer_to_bytes(const yuan::buffer::ByteBuffer & buf)
    {
        auto span = buf.readable_span();
        return std::vector<uint8_t>(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());
    }

    std::optional<std::vector<uint8_t>> load_private_key_der_any(const std::string & path)
    {
        auto raw = load_file_bytes(path);
        if (!raw || raw->empty()) {
            return std::nullopt;
        }

        if (raw->size() > 16 && (*raw)[0] == 0x30) {
            return raw;
        }

        BIO * bio = BIO_new_mem_buf(raw->data(), static_cast<int>(raw->size()));
        if (!bio) {
            return std::nullopt;
        }

        EVP_PKEY * pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) {
            return std::nullopt;
        }

        uint8_t * der = nullptr;
        const int der_len = i2d_PrivateKey(pkey, &der);
        EVP_PKEY_free(pkey);
        if (der_len <= 0 || !der) {
            return std::nullopt;
        }

        std::vector<uint8_t> der_vec(der, der + der_len);
        OPENSSL_free(der);
        return der_vec;
    }

    std::optional<std::vector<uint8_t>> build_public_key_blob_from_private_der(const std::vector<uint8_t> & private_key_der,
                                                                                std::string & algorithm_out)
    {
        const uint8_t * p = private_key_der.data();
        EVP_PKEY * pkey = d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(private_key_der.size()));
        if (!pkey) {
            return std::nullopt;
        }

        std::optional<std::vector<uint8_t>> result;
        const int type = EVP_PKEY_base_id(pkey);
        if (type == EVP_PKEY_ED25519) {
            size_t pub_len = 32;
            std::vector<uint8_t> pub(pub_len);
            if (EVP_PKEY_get_octet_string_param(
                    pkey,
                    OSSL_PKEY_PARAM_PUB_KEY,
                    pub.data(),
                    pub_len,
                    &pub_len) == 1) {
                pub.resize(pub_len);
                yuan::buffer::ByteBuffer buf;
                yuan::net::ssh::SshMessageCodec::write_string(buf, "ssh-ed25519");
                yuan::net::ssh::SshMessageCodec::write_raw(buf, pub.data(), pub.size());
                algorithm_out = "ssh-ed25519";
                result = buffer_to_bytes(buf);
            }
        } else if (type == EVP_PKEY_RSA) {
            BIGNUM * n = nullptr;
            BIGNUM * e = nullptr;
            EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
            EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);
            if (n && e) {
                std::vector<uint8_t> modulus(BN_num_bytes(n));
                std::vector<uint8_t> exponent(BN_num_bytes(e));
                BN_bn2bin(n, modulus.data());
                BN_bn2bin(e, exponent.data());

                yuan::buffer::ByteBuffer buf;
                yuan::net::ssh::SshMessageCodec::write_string(buf, "ssh-rsa");
                yuan::net::ssh::SshMessageCodec::write_mpint(buf, exponent);
                yuan::net::ssh::SshMessageCodec::write_mpint(buf, modulus);
                algorithm_out = "rsa-sha2-256";
                result = buffer_to_bytes(buf);
            }
            BN_free(n);
            BN_free(e);
        } else if (type == EVP_PKEY_EC) {
            size_t pub_len = 0;
            EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &pub_len);
            std::vector<uint8_t> pub(pub_len);
            if (pub_len > 0 &&
                EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY, pub.data(), pub_len, &pub_len) == 1) {
                pub.resize(pub_len);

                char group_name[32] = {};
                size_t group_len = sizeof(group_name);
                if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, group_name, sizeof(group_name), &group_len) == 1) {
                    std::string curve;
                    std::string algorithm;
                    if (std::string(group_name) == "prime256v1") {
                        curve = "nistp256";
                        algorithm = "ecdsa-sha2-nistp256";
                    } else if (std::string(group_name) == "secp384r1") {
                        curve = "nistp384";
                        algorithm = "ecdsa-sha2-nistp384";
                    } else if (std::string(group_name) == "secp521r1") {
                        curve = "nistp521";
                        algorithm = "ecdsa-sha2-nistp521";
                    }

                    if (!curve.empty()) {
                        yuan::buffer::ByteBuffer buf;
                        yuan::net::ssh::SshMessageCodec::write_string(buf, algorithm);
                        yuan::net::ssh::SshMessageCodec::write_string(buf, curve);
                        yuan::net::ssh::SshMessageCodec::write_string(buf, std::string(
                            reinterpret_cast<const char *>(pub.data()),
                            pub.size()));
                        algorithm_out = algorithm;
                        result = buffer_to_bytes(buf);
                    }
                }
            }
        }

        EVP_PKEY_free(pkey);
        return result;
    }

    std::vector<uint8_t> make_publickey_method_data(const std::string & algorithm,
                                                    const std::vector<uint8_t> & public_key_blob,
                                                    bool include_signature,
                                                    const std::vector<uint8_t> & signature)
    {
        yuan::buffer::ByteBuffer method;
        yuan::net::ssh::SshMessageCodec::write_boolean(method, include_signature);
        yuan::net::ssh::SshMessageCodec::write_string(method, algorithm);
        yuan::net::ssh::SshMessageCodec::write_raw(method, public_key_blob.data(), public_key_blob.size());
        if (include_signature) {
            yuan::net::ssh::SshMessageCodec::write_raw(method, signature.data(), signature.size());
        }
        return buffer_to_bytes(method);
    }

    std::optional<std::vector<uint8_t>> make_publickey_signature(const std::vector<uint8_t> & session_id,
                                                                 const std::string & username,
                                                                 const std::string & algorithm,
                                                                 const std::vector<uint8_t> & public_key_blob,
                                                                 const std::vector<uint8_t> & private_key_der,
                                                                 yuan::net::ssh::SshCryptoOpenSSL & crypto)
    {
        yuan::buffer::ByteBuffer signed_data;
        yuan::net::ssh::SshMessageCodec::write_raw(signed_data, session_id.data(), session_id.size());
        signed_data.append_u8(static_cast<uint8_t>(yuan::net::ssh::SshMessageType::SSH_MSG_USERAUTH_REQUEST));
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, username);
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, yuan::net::ssh::SSH_SERVICE_CONNECTION);
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, "publickey");
        yuan::net::ssh::SshMessageCodec::write_boolean(signed_data, true);
        yuan::net::ssh::SshMessageCodec::write_string(signed_data, algorithm);
        yuan::net::ssh::SshMessageCodec::write_raw(signed_data, public_key_blob.data(), public_key_blob.size());

        auto signed_data_bytes = buffer_to_bytes(signed_data);
        std::vector<uint8_t> raw_sig;
        if (algorithm == "ssh-ed25519") {
            const uint8_t * p = private_key_der.data();
            EVP_PKEY * pkey = d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(private_key_der.size()));
            if (!pkey) {
                return std::nullopt;
            }

            size_t priv_len = 32;
            std::vector<uint8_t> raw_priv(priv_len);
            if (EVP_PKEY_get_octet_string_param(
                    pkey,
                    OSSL_PKEY_PARAM_PRIV_KEY,
                    raw_priv.data(),
                    raw_priv.size(),
                    &priv_len) != 1) {
                EVP_PKEY_free(pkey);
                return std::nullopt;
            }
            raw_priv.resize(priv_len);
            EVP_PKEY_free(pkey);
            raw_sig = crypto.ed25519_sign(raw_priv, signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "rsa-sha2-256") {
            raw_sig = crypto.rsa_sign(private_key_der, "sha256", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "rsa-sha2-512") {
            raw_sig = crypto.rsa_sign(private_key_der, "sha512", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "ecdsa-sha2-nistp256") {
            raw_sig = crypto.ecdsa_sign(private_key_der, "P-256", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "ecdsa-sha2-nistp384") {
            raw_sig = crypto.ecdsa_sign(private_key_der, "P-384", signed_data_bytes.data(), signed_data_bytes.size());
        } else if (algorithm == "ecdsa-sha2-nistp521") {
            raw_sig = crypto.ecdsa_sign(private_key_der, "P-521", signed_data_bytes.data(), signed_data_bytes.size());
        }

        if (raw_sig.empty()) {
            return std::nullopt;
        }

        yuan::buffer::ByteBuffer signature_field;
        yuan::net::ssh::SshMessageCodec::write_string(signature_field, algorithm);
        yuan::net::ssh::SshMessageCodec::write_string(signature_field, std::string(
            reinterpret_cast<const char *>(raw_sig.data()),
            raw_sig.size()));
        return buffer_to_bytes(signature_field);
    }

    std::vector<uint8_t> make_exec_request_data(const std::string &command)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_string(data, command);
        return {
            reinterpret_cast<const uint8_t *>(data.read_ptr()),
            reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes()
        };
    }

    std::vector<uint8_t> make_pty_request_data()
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_string(data, "xterm-256color");
        yuan::net::ssh::SshMessageCodec::write_uint32(data, 120);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, 40);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, 0);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, 0);
        yuan::net::ssh::SshMessageCodec::write_string(data, std::string());
        return {
            reinterpret_cast<const uint8_t *>(data.read_ptr()),
            reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes()
        };
    }

    std::vector<uint8_t> make_window_change_request_data(uint32_t cols,
                                                         uint32_t rows,
                                                         uint32_t pixel_width,
                                                         uint32_t pixel_height)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_uint32(data, cols);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, rows);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, pixel_width);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, pixel_height);
        return {
            reinterpret_cast<const uint8_t *>(data.read_ptr()),
            reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes()
        };
    }

    std::vector<uint8_t> make_signal_request_data(const std::string & signal_name)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_string(data, signal_name);
        return {
            reinterpret_cast<const uint8_t *>(data.read_ptr()),
            reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes()
        };
    }

    struct RemoteForwardSpec
    {
        std::string bind_addr;
        uint16_t bind_port = 0;
        std::string target_host;
        uint16_t target_port = 0;
    };

    std::optional<RemoteForwardSpec> parse_remote_forward_spec(const std::string & raw);
    std::optional<uint16_t> parse_u16_strict(const std::string & text);

    using LocalForwardSpec = RemoteForwardSpec;

    std::optional<LocalForwardSpec> parse_local_forward_spec(const std::string & raw)
    {
        return parse_remote_forward_spec(raw);
    }

    struct DynamicForwardSpec
    {
        std::string bind_addr;
        uint16_t bind_port = 0;
    };

    std::optional<DynamicForwardSpec> parse_dynamic_forward_spec(const std::string & raw)
    {
        if (raw.empty()) {
            return std::nullopt;
        }

        DynamicForwardSpec spec;
        const size_t colon = raw.rfind(':');
        if (colon == std::string::npos) {
            auto port = parse_u16_strict(raw);
            if (!port || *port == 0) {
                return std::nullopt;
            }
            spec.bind_addr = "127.0.0.1";
            spec.bind_port = *port;
            return spec;
        }

        const std::string addr = raw.substr(0, colon);
        const std::string port_text = raw.substr(colon + 1);
        auto port = parse_u16_strict(port_text);
        if (!port || *port == 0) {
            return std::nullopt;
        }

        spec.bind_addr = addr.empty() ? "127.0.0.1" : addr;
        spec.bind_port = *port;
        return spec;
    }

    std::optional<uint16_t> parse_u16_strict(const std::string & text)
    {
        if (text.empty()) {
            return std::nullopt;
        }
        uint32_t value = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<uint32_t>(ch - '0');
            if (value > 65535) {
                return std::nullopt;
            }
        }
        return static_cast<uint16_t>(value);
    }

    std::optional<RemoteForwardSpec> parse_remote_forward_spec(const std::string & raw)
    {
        const size_t c1 = raw.find(':');
        if (c1 == std::string::npos) {
            return std::nullopt;
        }
        const size_t c2 = raw.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            return std::nullopt;
        }

        std::string first = raw.substr(0, c1);
        std::string second = raw.substr(c1 + 1, c2 - c1 - 1);
        std::string third = raw.substr(c2 + 1);

        if (second.empty() || third.empty()) {
            return std::nullopt;
        }

        const size_t c3 = third.rfind(':');
        if (c3 == std::string::npos || c3 == 0 || c3 + 1 >= third.size()) {
            return std::nullopt;
        }

        std::string target_host = third.substr(0, c3);
        std::string target_port_str = third.substr(c3 + 1);
        auto target_port = parse_u16_strict(target_port_str);
        if (!target_port || *target_port == 0) {
            return std::nullopt;
        }

        RemoteForwardSpec spec;
        if (auto maybe_mid_port = parse_u16_strict(second); maybe_mid_port) {
            spec.bind_addr = first.empty() ? "127.0.0.1" : first;
            spec.bind_port = *maybe_mid_port;
            if (spec.bind_port == 0) {
                return std::nullopt;
            }
            spec.target_host = target_host;
            spec.target_port = *target_port;
            return spec;
        }

        auto bind_port = parse_u16_strict(first);
        if (!bind_port || *bind_port == 0) {
            return std::nullopt;
        }

        spec.bind_addr = "127.0.0.1";
        spec.bind_port = *bind_port;
        spec.target_host = second;
        spec.target_port = *target_port;
        return spec;
    }

    struct TerminalSize
    {
        uint32_t cols = 120;
        uint32_t rows = 40;
        uint32_t pixel_width = 0;
        uint32_t pixel_height = 0;
    };

    TerminalSize query_terminal_size()
    {
        TerminalSize ts;
#ifndef _WIN32
        winsize ws{};
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_col > 0) {
                ts.cols = ws.ws_col;
            }
            if (ws.ws_row > 0) {
                ts.rows = ws.ws_row;
            }
            ts.pixel_width = ws.ws_xpixel;
            ts.pixel_height = ws.ws_ypixel;
        }
#endif
        return ts;
    }

    yuan::buffer::ByteBuffer encode_channel_data_packet(uint32_t recipient_channel, const std::string & text)
    {
        yuan::net::ssh::SshChannelDataMessage msg;
        msg.recipient_channel = recipient_channel;
        msg.data.assign(text.begin(), text.end());
        return yuan::net::ssh::SshMessageCodec::encode_channel_data(msg);
    }

    bool run_exec_phase1(SocketHandle fd, const CliArgs &args)
    {
        using namespace yuan::net::ssh;

        bool stderr_line_start = true;
        auto write_stderr = [&](const std::vector<uint8_t> &data) {
            if (data.empty()) {
                return;
            }
            if (!args.stderr_prefix) {
                std::cerr.write(reinterpret_cast<const char *>(data.data()),
                                static_cast<std::streamsize>(data.size()));
                std::cerr.flush();
                return;
            }

            constexpr const char *kPrefix = "[stderr] ";
            for (uint8_t byte : data) {
                if (stderr_line_start) {
                    std::cerr << kPrefix;
                    stderr_line_start = false;
                }
                std::cerr.put(static_cast<char>(byte));
                if (byte == '\n') {
                    stderr_line_start = true;
                }
            }
            std::cerr.flush();
        };

#ifndef _WIN32
        const int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        const bool stdin_nonblock_enabled = stdin_flags >= 0;
        struct StdinFlagsGuard
        {
            bool enabled = false;
            int original_flags = 0;
            ~StdinFlagsGuard()
            {
                if (enabled) {
                    (void)fcntl(STDIN_FILENO, F_SETFL, original_flags);
                }
            }
        } stdin_guard;
        stdin_guard.enabled = stdin_nonblock_enabled;
        stdin_guard.original_flags = stdin_flags;
        if (stdin_nonblock_enabled) {
            (void)fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
        }
#endif

        SshCryptoOpenSSL crypto;
        SshAlgorithmRegistry registry;
        registry.register_defaults();

        SshTransport transport(&registry, &crypto, false);
        transport.set_we_are_server(false);

        auto debug = [&](const std::string &message) {
            if (args.verbose > 0) {
                std::cerr << "debug: " << message << '\n';
            }
        };

        const std::string client_version = "SSH-2.0-YuanReleaseCliExec_0.1\r\n";
        if (!send_all(fd,
                      reinterpret_cast<const uint8_t *>(client_version.data()),
                      client_version.size())) {
            std::cerr << "failed to send client version\n";
            return false;
        }

        std::string server_version_line;
        if (!recv_line(fd, server_version_line)) {
            std::cerr << "failed to read server version\n";
            return false;
        }

        auto server_info = SshVersionExchange::parse_version_line(server_version_line);
        if (!server_info || !SshVersionExchange::is_valid_protocol_version(server_info->protocol_version)) {
            std::cerr << "invalid server version line\n";
            return false;
        }

        transport.set_client_version(client_version.substr(0, client_version.size() - 2));
        transport.set_server_version(server_info->raw_line);
        debug("version exchange complete: " + server_info->raw_line);

        SshServerConfig client_cfg;
        client_cfg.kex_algorithms = {
            "curve25519-sha256",
            "curve25519-sha256@libssh.org"
        };
        client_cfg.host_key_algorithms = {
            "ssh-ed25519",
            "rsa-sha2-512",
            "rsa-sha2-256"
        };
        client_cfg.cipher_algorithms = {
            "chacha20-poly1305@openssh.com",
            "aes256-ctr",
            "aes128-ctr"
        };
        client_cfg.mac_algorithms = {
            "hmac-sha2-256",
            "hmac-sha2-512"
        };
        client_cfg.compression_algorithms = { "none" };

        auto our_kex = transport.build_kex_init(client_cfg);
        if (!send_packet(fd, transport, our_kex)) {
            std::cerr << "failed to send kexinit\n";
            return false;
        }
        debug("sent KEXINIT");

        yuan::buffer::ByteBuffer recv_buf;
        bool got_peer_kexinit = false;
        bool sent_kex_init = false;
        bool sent_newkeys = false;
        bool got_newkeys = false;

        while (!got_newkeys) {
            std::vector<uint8_t> payload;
            const auto read_status = read_packet(fd, recv_buf, transport, payload);
            if (read_status == PacketReadStatus::timeout) {
                continue;
            }
            if (read_status != PacketReadStatus::ok) {
                std::cerr << "failed while reading kex packets\n";
                return false;
            }
            if (payload.empty()) {
                continue;
            }

            const auto msg_type = static_cast<SshMessageType>(payload[0]);
            debug("kex packet type " + std::to_string(static_cast<int>(payload[0])));
            if (msg_type == SshMessageType::SSH_MSG_KEXINIT) {
                auto kex_init = SshMessageCodec::decode_kex_init(payload.data(), payload.size());
                if (!kex_init) {
                    std::cerr << "invalid peer kexinit\n";
                    return false;
                }
                transport.set_peer_kex_init_raw(payload);

                auto negotiated = transport.process_kex_init(*kex_init, client_cfg);
                if (!negotiated) {
                    std::cerr << "failed to negotiate algorithms\n";
                    return false;
                }
                auto host_key_algo = registry.create_host_key(negotiated->host_key_name);
                if (!host_key_algo) {
                    std::cerr << "unsupported host key algorithm: " << negotiated->host_key_name << '\n';
                    return false;
                }
                host_key_algo->set_crypto(&crypto);
                transport.set_host_key_algorithm(std::move(host_key_algo));
                got_peer_kexinit = true;

                auto client_pub = transport.generate_kex_public_key();
                if (!client_pub || client_pub->empty()) {
                    std::cerr << "failed to generate client kex public key\n";
                    return false;
                }

                SshKexEcdhInitMessage init_msg;
                init_msg.client_public_key = std::move(*client_pub);
                if (!send_packet(fd, transport, SshMessageCodec::encode_kex_ecdh_init(init_msg))) {
                    std::cerr << "failed to send ecdh init\n";
                    return false;
                }
                sent_kex_init = true;
                debug("sent ECDH init");
            } else if (msg_type == SshMessageType::SSH_MSG_KEX_ECDH_REPLY ||
                       msg_type == SshMessageType::SSH_MSG_KEXDH_REPLY) {
                if (!sent_kex_init) {
                    std::cerr << "received kex reply before ecdh init\n";
                    return false;
                }
                auto reply = SshMessageCodec::decode_kex_ecdh_reply(payload.data(), payload.size());
                if (!reply) {
                    std::cerr << "invalid kex reply\n";
                    return false;
                }

                if (!transport.process_kex_reply_message(*reply,
                                                         transport.client_version(),
                                                         transport.server_version())) {
                    std::cerr << "failed to process kex reply\n";
                    return false;
                }

                std::string known_hosts_error;
                if (!check_known_hosts(args, reply->host_key_blob, known_hosts_error)) {
                    std::cerr << known_hosts_error << '\n';
                    return false;
                }

                if (!send_packet(fd, transport, SshMessageCodec::encode_newkeys())) {
                    std::cerr << "failed to send NEWKEYS\n";
                    return false;
                }
                sent_newkeys = true;
                debug("sent NEWKEYS");
            } else if (msg_type == SshMessageType::SSH_MSG_NEWKEYS) {
                if (!sent_newkeys) {
                    std::cerr << "received NEWKEYS before sending NEWKEYS\n";
                    return false;
                }
                if (!transport.process_newkeys()) {
                    std::cerr << "failed to activate new keys\n";
                    return false;
                }
                got_newkeys = true;
                debug("received NEWKEYS and activated keys");
            }
        }

        SshServiceRequestMessage service_req;
        service_req.service_name = SSH_SERVICE_USERAUTH;
        if (!send_packet(fd, transport, SshMessageCodec::encode_service_request(service_req))) {
            std::cerr << "failed to send service request\n";
            return false;
        }
        debug("sent service request");

        const bool interactive_mode = args.command.empty();
        bool service_accepted = false;
        bool auth_ok = false;
        bool attempted_publickey = false;
        bool waiting_pk_ok = false;
        std::string publickey_algorithm;
        std::vector<uint8_t> publickey_blob;
        std::vector<uint8_t> private_key_der;

        if (!args.identity_files.empty()) {
            const auto maybe_der = load_private_key_der_any(args.identity_files.front());
            if (!maybe_der) {
                std::cerr << "failed to load private key: " << args.identity_files.front() << '\n';
                return false;
            }
            private_key_der = *maybe_der;
            auto maybe_blob = build_public_key_blob_from_private_der(private_key_der, publickey_algorithm);
            if (!maybe_blob) {
                std::cerr << "failed to derive SSH public key blob from private key\n";
                return false;
            }
            publickey_blob = *maybe_blob;
        }
        bool channel_opened = false;
        bool exec_sent = false;
        bool shell_sent = false;
        bool pty_sent = false;
        bool pty_ok = false;
        bool stdin_eof_sent = false;
        bool shell_ready = false;
        TerminalSize last_terminal_size = query_terminal_size();
#ifndef _WIN32
        sig_atomic_t handled_sigint_count = 0;
#endif
        uint32_t local_channel_id = 0;
        uint32_t remote_channel_id = 0;
        uint32_t exit_code = 0;
        bool got_exit_status = false;
        std::vector<LocalForwardSpec> local_forward_specs;
        std::vector<DynamicForwardSpec> dynamic_forward_specs;
        std::vector<RemoteForwardSpec> remote_forward_specs;
        std::vector<RemoteForwardSpec> pending_remote_forward_specs;
        uint32_t next_forward_local_channel_id = 1024;
        std::unordered_map<std::string, SocketHandle> local_forward_listeners;
        std::unordered_map<std::string, SocketHandle> dynamic_forward_listeners;
        std::unordered_map<uint32_t, SocketHandle> pending_local_open_socket;
        std::unordered_map<uint32_t, SocketHandle> pending_dynamic_open_socket;
        std::unordered_map<uint32_t, SocketHandle> forward_local_to_socket;
        std::unordered_map<uint32_t, uint32_t> forward_local_to_remote;
        std::unordered_map<uint32_t, uint32_t> forward_remote_to_local;
        struct PendingSocksClient
        {
            SocketHandle socket = kInvalidSocket;
            std::string origin_host;
            uint16_t origin_port = 0;
            std::vector<uint8_t> recv_buf;
            bool method_negotiated = false;
        };
        std::unordered_map<SocketHandle, PendingSocksClient> pending_socks_clients;
        for (const auto &raw : args.local_forwards) {
            auto spec = parse_local_forward_spec(raw);
            if (!spec) {
                std::cerr << "invalid -L spec: " << raw << '\n';
                return false;
            }
            local_forward_specs.push_back(*spec);

            SocketHandle listen_fd = kInvalidSocket;
            if (!create_listen_socket(spec->bind_addr, spec->bind_port, listen_fd)) {
                std::cerr << "failed to create -L listener on "
                          << spec->bind_addr << ':' << spec->bind_port << '\n';
                return false;
            }
            const std::string key = spec->bind_addr + ":" + std::to_string(spec->bind_port);
            local_forward_listeners[key] = listen_fd;
        }
        for (const auto &raw : args.dynamic_forwards) {
            auto spec = parse_dynamic_forward_spec(raw);
            if (!spec) {
                std::cerr << "invalid -D spec: " << raw << '\n';
                return false;
            }
            dynamic_forward_specs.push_back(*spec);

            SocketHandle listen_fd = kInvalidSocket;
            if (!create_listen_socket(spec->bind_addr, spec->bind_port, listen_fd)) {
                std::cerr << "failed to create -D listener on "
                          << spec->bind_addr << ':' << spec->bind_port << '\n';
                return false;
            }
            const std::string key = spec->bind_addr + ":" + std::to_string(spec->bind_port);
            dynamic_forward_listeners[key] = listen_fd;
        }
        for (const auto &raw : args.remote_forwards) {
            auto spec = parse_remote_forward_spec(raw);
            if (!spec) {
                std::cerr << "invalid -R spec: " << raw << '\n';
                return false;
            }
            pending_remote_forward_specs.push_back(*spec);
        }

        {
            int io_poll_timeout_ms = args.timeout_ms;
            const bool need_responsive_poll =
                interactive_mode ||
                !local_forward_specs.empty() ||
                !dynamic_forward_specs.empty() ||
                !pending_remote_forward_specs.empty();
            if (need_responsive_poll && io_poll_timeout_ms > 100) {
                io_poll_timeout_ms = 100;
            }
            if (io_poll_timeout_ms < 100) {
                io_poll_timeout_ms = 100;
            }
            (void)set_recv_timeout(fd, io_poll_timeout_ms);
        }

        auto close_forward_channel = [&](uint32_t local_id) {
            auto sock_it = forward_local_to_socket.find(local_id);
            if (sock_it != forward_local_to_socket.end()) {
                close_socket_handle(sock_it->second);
                forward_local_to_socket.erase(sock_it);
            }
            auto remote_it = forward_local_to_remote.find(local_id);
            if (remote_it != forward_local_to_remote.end()) {
                forward_remote_to_local.erase(remote_it->second);
                forward_local_to_remote.erase(remote_it);
            }
        };

        auto cleanup_forward_resources = [&]() {
            for (auto &it : local_forward_listeners) {
                close_socket_handle(it.second);
            }
            for (auto &it : dynamic_forward_listeners) {
                close_socket_handle(it.second);
            }
            for (auto &it : pending_local_open_socket) {
                close_socket_handle(it.second);
            }
            for (auto &it : pending_dynamic_open_socket) {
                close_socket_handle(it.second);
            }
            for (auto &it : pending_socks_clients) {
                close_socket_handle(it.second.socket);
            }
        };

        auto pump_local_forward_accepts = [&]() -> bool {
            if (!auth_ok || local_forward_specs.empty()) {
                return true;
            }

            const size_t kMaxPendingForwards = 8;
            if (pending_local_open_socket.size() + pending_dynamic_open_socket.size() >= kMaxPendingForwards) {
                return true;
            }

            for (const auto &spec : local_forward_specs) {
                const std::string key = spec.bind_addr + ":" + std::to_string(spec.bind_port);
                auto listener_it = local_forward_listeners.find(key);
                if (listener_it == local_forward_listeners.end()) {
                    continue;
                }

                std::string origin_host;
                uint16_t origin_port = 0;
                bool had_fatal_error = false;
                SocketHandle accepted_fd = kInvalidSocket;
                const bool accepted = accept_pending_client(
                    listener_it->second,
                    accepted_fd,
                    origin_host,
                    origin_port,
                    had_fatal_error);

                if (had_fatal_error) {
                    std::cerr << "local forward listener failure on " << key << '\n';
                    return false;
                }
                if (!accepted) {
                    continue;
                }

                SshChannelOpenMessage open_msg;
                const uint32_t local_forward_id = next_forward_local_channel_id++;
                open_msg.channel_type = SSH_CHANNEL_DIRECT_TCPIP;
                open_msg.sender_channel = local_forward_id;
                open_msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
                open_msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;

                yuan::buffer::ByteBuffer open_data;
                SshMessageCodec::write_string(open_data, spec.target_host);
                SshMessageCodec::write_uint32(open_data, spec.target_port);
                SshMessageCodec::write_string(open_data, origin_host);
                SshMessageCodec::write_uint32(open_data, origin_port);
                open_msg.type_specific_data.assign(
                    reinterpret_cast<const uint8_t *>(open_data.read_ptr()),
                    reinterpret_cast<const uint8_t *>(open_data.read_ptr()) + open_data.readable_bytes());

                if (!send_packet(fd, transport, SshMessageCodec::encode_channel_open(open_msg))) {
                    close_socket_handle(accepted_fd);
                    return false;
                }
                debug("local-forward open sent local=" + std::to_string(local_forward_id));
                pending_local_open_socket[local_forward_id] = accepted_fd;

                if (pending_local_open_socket.size() + pending_dynamic_open_socket.size() >= kMaxPendingForwards) {
                    break;
                }
            }
            return true;
        };

        auto close_socks_client = [&](SocketHandle sock) {
            auto it = pending_socks_clients.find(sock);
            if (it != pending_socks_clients.end()) {
                close_socket_handle(it->second.socket);
                pending_socks_clients.erase(it);
                return;
            }
            close_socket_handle(sock);
        };

        auto send_socks_reply = [&](SocketHandle sock, uint8_t rep_code) -> bool {
            const std::array<uint8_t, 10> reply = {
                0x05,
                rep_code,
                0x00,
                0x01,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00
            };
            return send_all(sock, reply.data(), reply.size());
        };

        auto pump_dynamic_forward_accepts = [&]() -> bool {
            if (!auth_ok || dynamic_forward_specs.empty()) {
                return true;
            }

            const size_t kMaxPendingForwards = 8;
            if (pending_local_open_socket.size() + pending_dynamic_open_socket.size() >= kMaxPendingForwards) {
                return true;
            }

            for (const auto &spec : dynamic_forward_specs) {
                const std::string key = spec.bind_addr + ":" + std::to_string(spec.bind_port);
                auto listener_it = dynamic_forward_listeners.find(key);
                if (listener_it == dynamic_forward_listeners.end()) {
                    continue;
                }

                std::string origin_host;
                uint16_t origin_port = 0;
                bool had_fatal_error = false;
                SocketHandle accepted_fd = kInvalidSocket;
                const bool accepted = accept_pending_client(
                    listener_it->second,
                    accepted_fd,
                    origin_host,
                    origin_port,
                    had_fatal_error);
                if (had_fatal_error) {
                    std::cerr << "dynamic forward listener failure on " << key << '\n';
                    return false;
                }
                if (!accepted) {
                    continue;
                }

                PendingSocksClient client;
                client.socket = accepted_fd;
                client.origin_host = origin_host;
                client.origin_port = origin_port;
                pending_socks_clients[accepted_fd] = std::move(client);
                debug("dynamic-forward accepted client fd=" + std::to_string(static_cast<int>(accepted_fd)) +
                      " origin=" + origin_host + ":" + std::to_string(origin_port));

                if (pending_local_open_socket.size() + pending_dynamic_open_socket.size() >= kMaxPendingForwards) {
                    break;
                }
            }
            return true;
        };

        auto pump_dynamic_socks_handshake = [&]() -> bool {
            if (pending_socks_clients.empty()) {
                return true;
            }

            std::vector<SocketHandle> sockets;
            sockets.reserve(pending_socks_clients.size());
            for (const auto &it : pending_socks_clients) {
                sockets.push_back(it.first);
            }

            for (SocketHandle sock : sockets) {
                auto client_it = pending_socks_clients.find(sock);
                if (client_it == pending_socks_clients.end()) {
                    continue;
                }

                if (!socket_read_ready(sock)) {
                    continue;
                }

                std::array<uint8_t, 4096> chunk{};
#ifdef _WIN32
                const int n = recv(sock, reinterpret_cast<char *>(chunk.data()), static_cast<int>(chunk.size()), 0);
#else
                const ssize_t n = recv(sock, chunk.data(), chunk.size(), 0);
#endif
                if (n == 0) {
                    close_socks_client(sock);
                    continue;
                }
                if (n < 0) {
                    if (socket_would_block_last_error()) {
                        continue;
                    }
                    close_socks_client(sock);
                    continue;
                }

                auto &client = client_it->second;
                client.recv_buf.insert(client.recv_buf.end(), chunk.begin(), chunk.begin() + n);

                while (!client.recv_buf.empty()) {
                    if (!client.method_negotiated &&
                        client.recv_buf.size() >= 2 &&
                        client.recv_buf[0] == 0x05) {
                        const size_t nmethods = client.recv_buf[1];
                        const size_t method_len = 2 + nmethods;
                        if (client.recv_buf.size() >= method_len) {
                            bool supports_no_auth = false;
                            for (size_t i = 0; i < nmethods; ++i) {
                                if (client.recv_buf[2 + i] == 0x00) {
                                    supports_no_auth = true;
                                    break;
                                }
                            }

                            const std::array<uint8_t, 2> method_reply = {
                                0x05,
                                static_cast<uint8_t>(supports_no_auth ? 0x00 : 0xFF)
                            };
                            if (!send_all(client.socket, method_reply.data(), method_reply.size())) {
                                close_socks_client(sock);
                                break;
                            }
                            client.recv_buf.erase(client.recv_buf.begin(), client.recv_buf.begin() + static_cast<std::ptrdiff_t>(method_len));
                            if (!supports_no_auth) {
                                close_socks_client(sock);
                                break;
                            }
                            client.method_negotiated = true;
                            debug("dynamic-forward socks method negotiation ok fd=" +
                                  std::to_string(static_cast<int>(client.socket)));
                            continue;
                        }
                    }

                    if (client.recv_buf.size() < 4) {
                        break;
                    }

                    const uint8_t ver = client.recv_buf[0];
                    const uint8_t cmd = client.recv_buf[1];
                    const uint8_t rsv = client.recv_buf[2];
                    const uint8_t atyp = client.recv_buf[3];
                    if (ver != 0x05 || rsv != 0x00) {
                        (void)send_socks_reply(client.socket, 0x01);
                        close_socks_client(sock);
                        break;
                    }
                    if (cmd != 0x01) {
                        (void)send_socks_reply(client.socket, 0x07);
                        close_socks_client(sock);
                        break;
                    }

                    size_t offset = 4;
                    std::string target_host;
                    if (atyp == 0x01) {
                        if (client.recv_buf.size() < offset + 4 + 2) {
                            break;
                        }
                        target_host = std::to_string(client.recv_buf[offset]) + "." +
                                      std::to_string(client.recv_buf[offset + 1]) + "." +
                                      std::to_string(client.recv_buf[offset + 2]) + "." +
                                      std::to_string(client.recv_buf[offset + 3]);
                        offset += 4;
                    } else if (atyp == 0x03) {
                        if (client.recv_buf.size() < offset + 1) {
                            break;
                        }
                        const size_t host_len = client.recv_buf[offset++];
                        if (client.recv_buf.size() < offset + host_len + 2) {
                            break;
                        }
                        target_host.assign(reinterpret_cast<const char *>(&client.recv_buf[offset]), host_len);
                        offset += host_len;
                    } else if (atyp == 0x04) {
                        if (client.recv_buf.size() < offset + 16 + 2) {
                            break;
                        }
                        char host_text[INET6_ADDRSTRLEN] = {};
                        if (!inet_ntop(AF_INET6, client.recv_buf.data() + offset, host_text, sizeof(host_text))) {
                            (void)send_socks_reply(client.socket, 0x08);
                            close_socks_client(sock);
                            break;
                        }
                        target_host = host_text;
                        offset += 16;
                    } else {
                        (void)send_socks_reply(client.socket, 0x08);
                        close_socks_client(sock);
                        break;
                    }

                    if (client.recv_buf.size() < offset + 2) {
                        break;
                    }
                    const uint16_t target_port =
                        static_cast<uint16_t>((static_cast<uint16_t>(client.recv_buf[offset]) << 8u) |
                                              static_cast<uint16_t>(client.recv_buf[offset + 1]));
                    client.recv_buf.erase(client.recv_buf.begin(), client.recv_buf.begin() + static_cast<std::ptrdiff_t>(offset + 2));

                    SshChannelOpenMessage open_msg;
                    const uint32_t local_forward_id = next_forward_local_channel_id++;
                    open_msg.channel_type = SSH_CHANNEL_DIRECT_TCPIP;
                    open_msg.sender_channel = local_forward_id;
                    open_msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
                    open_msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;

                    yuan::buffer::ByteBuffer open_data;
                    SshMessageCodec::write_string(open_data, target_host);
                    SshMessageCodec::write_uint32(open_data, target_port);
                    SshMessageCodec::write_string(open_data, client.origin_host);
                    SshMessageCodec::write_uint32(open_data, client.origin_port);
                    open_msg.type_specific_data.assign(
                        reinterpret_cast<const uint8_t *>(open_data.read_ptr()),
                        reinterpret_cast<const uint8_t *>(open_data.read_ptr()) + open_data.readable_bytes());

                    if (!send_packet(fd, transport, SshMessageCodec::encode_channel_open(open_msg))) {
                        close_socks_client(sock);
                        return false;
                    }
                    debug("dynamic-forward open sent local=" + std::to_string(local_forward_id) +
                          " target=" + target_host + ":" + std::to_string(target_port));

                    pending_dynamic_open_socket[local_forward_id] = client.socket;
                    pending_socks_clients.erase(client_it);
                    break;
                }
            }

            return true;
        };

        auto pump_forward_target_reads = [&]() -> bool {
            std::vector<uint32_t> local_ids;
            local_ids.reserve(forward_local_to_socket.size());
            for (const auto &it : forward_local_to_socket) {
                local_ids.push_back(it.first);
            }

            std::array<uint8_t, 64 * 1024> buffer{};
            for (uint32_t local_id : local_ids) {
                auto sock_it = forward_local_to_socket.find(local_id);
                if (sock_it == forward_local_to_socket.end()) {
                    continue;
                }
                if (!socket_read_ready(sock_it->second)) {
                    continue;
                }

#ifdef _WIN32
                const int n = recv(sock_it->second,
                                   reinterpret_cast<char *>(buffer.data()),
                                   static_cast<int>(buffer.size()),
                                   0);
#else
                const ssize_t n = recv(sock_it->second,
                                       buffer.data(),
                                       buffer.size(),
                                       0);
#endif
                if (n == 0) {
                    auto remote_it = forward_local_to_remote.find(local_id);
                    if (remote_it != forward_local_to_remote.end()) {
                        SshChannelEofMessage eof_msg;
                        eof_msg.recipient_channel = remote_it->second;
                        (void)send_packet(fd, transport, SshMessageCodec::encode_channel_eof(eof_msg));

                        SshChannelCloseMessage close_msg;
                        close_msg.recipient_channel = remote_it->second;
                        (void)send_packet(fd, transport, SshMessageCodec::encode_channel_close(close_msg));
                    }
                    close_forward_channel(local_id);
                    continue;
                }
                if (n < 0) {
                    if (socket_would_block_last_error()) {
                        continue;
                    }
                    auto remote_it = forward_local_to_remote.find(local_id);
                    if (remote_it != forward_local_to_remote.end()) {
                        SshChannelCloseMessage close_msg;
                        close_msg.recipient_channel = remote_it->second;
                        (void)send_packet(fd, transport, SshMessageCodec::encode_channel_close(close_msg));
                    }
                    close_forward_channel(local_id);
                    continue;
                }

                auto remote_it = forward_local_to_remote.find(local_id);
                if (remote_it == forward_local_to_remote.end()) {
                    continue;
                }

                SshChannelDataMessage data_msg;
                data_msg.recipient_channel = remote_it->second;
                data_msg.data.assign(buffer.begin(), buffer.begin() + n);
                if (!send_packet(fd, transport, SshMessageCodec::encode_channel_data(data_msg))) {
                    return false;
                }
                debug("local-forward send data bytes=" + std::to_string(static_cast<int>(n)) +
                      " local=" + std::to_string(local_id) +
                      " remote=" + std::to_string(remote_it->second));
            }
            return true;
        };

        while (true) {
            std::vector<uint8_t> payload;
            const auto read_status = read_packet(fd, recv_buf, transport, payload);
            if (read_status == PacketReadStatus::timeout) {
                if (interactive_mode && shell_sent && !stdin_eof_sent) {
                    std::string stdin_chunk;
                    const auto poll = read_stdin_chunk_nonblocking(stdin_chunk);
                    if (poll == StdinPollResult::data && !stdin_chunk.empty()) {
                        auto ch_data = encode_channel_data_packet(remote_channel_id, stdin_chunk);
                        if (!send_packet(fd, transport, ch_data)) {
                            std::cerr << "failed to send interactive input\n";
                            return false;
                        }
                    } else if (poll == StdinPollResult::eof) {
                        SshChannelEofMessage eof_msg;
                        eof_msg.recipient_channel = remote_channel_id;
                        (void)send_packet(fd, transport, SshMessageCodec::encode_channel_eof(eof_msg));
                        stdin_eof_sent = true;
                    } else if (poll == StdinPollResult::error) {
                        std::cerr << "failed to read interactive input\n";
                        return false;
                    }
                }
                if (!pump_forward_target_reads()) {
                    std::cerr << "failed to send forwarded data\n";
                    return false;
                }
                if (!pump_local_forward_accepts()) {
                    std::cerr << "failed to accept local forward clients\n";
                    return false;
                }
                if (!pump_dynamic_forward_accepts() || !pump_dynamic_socks_handshake()) {
                    std::cerr << "failed to process dynamic forwards\n";
                    return false;
                }
                continue;
            }
            if (read_status != PacketReadStatus::ok) {
                cleanup_forward_resources();
                std::cerr << "connection closed\n";
                return false;
            }
            if (payload.empty()) {
                continue;
            }

            const auto type = static_cast<SshMessageType>(payload[0]);
            debug("packet type " + std::to_string(static_cast<int>(payload[0])));
            if (type == SshMessageType::SSH_MSG_SERVICE_ACCEPT) {
                service_accepted = true;

                if (!publickey_blob.empty()) {
                    SshUserauthRequestMessage auth_req;
                    auth_req.username = args.user;
                    auth_req.service_name = SSH_SERVICE_CONNECTION;
                    auth_req.method_name = "publickey";
                    auth_req.method_specific_data = make_publickey_method_data(
                        publickey_algorithm,
                        publickey_blob,
                        false,
                        {});
                    if (!send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req))) {
                        std::cerr << "failed to send publickey probe auth request\n";
                        return false;
                    }
                    attempted_publickey = true;
                    waiting_pk_ok = true;
                    debug("sent publickey auth probe request");
                } else {
                    SshUserauthRequestMessage auth_req;
                    auth_req.username = args.user;
                    auth_req.service_name = SSH_SERVICE_CONNECTION;
                    auth_req.method_name = "password";
                    auth_req.method_specific_data = make_password_method_data(args.password);
                    if (!send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req))) {
                        std::cerr << "failed to send password auth request\n";
                        return false;
                    }
                    debug("sent password auth request");
                }
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_SUCCESS) {
                auth_ok = true;
                waiting_pk_ok = false;

                for (const auto &spec : pending_remote_forward_specs) {
                    SshGlobalRequestMessage req;
                    req.request_name = "tcpip-forward";
                    req.want_reply = true;
                    yuan::buffer::ByteBuffer data;
                    SshMessageCodec::write_string(data, spec.bind_addr);
                    SshMessageCodec::write_uint32(data, spec.bind_port);
                    req.request_specific_data.assign(
                        reinterpret_cast<const uint8_t *>(data.read_ptr()),
                        reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes());
                    if (!send_packet(fd, transport, SshMessageCodec::encode_global_request(req))) {
                        std::cerr << "failed to send tcpip-forward request\n";
                        return false;
                    }
                }

                SshChannelOpenMessage open_msg;
                open_msg.channel_type = SSH_CHANNEL_SESSION;
                open_msg.sender_channel = local_channel_id;
                open_msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
                open_msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;
                if (!send_packet(fd, transport, SshMessageCodec::encode_channel_open(open_msg))) {
                    std::cerr << "failed to open channel\n";
                    return false;
                }
                debug("sent channel open");
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_FAILURE) {
                auto fail = SshMessageCodec::decode_userauth_failure(payload.data(), payload.size());
                const std::string methods = fail ? fail->auth_methods_that_can_continue : std::string();

                if (attempted_publickey && args.password.empty()) {
                    std::cerr << "publickey authentication failed";
                    if (!methods.empty()) {
                        std::cerr << " (methods: " << methods << ')';
                    }
                    std::cerr << '\n';
                    return false;
                }

                if (attempted_publickey && !args.password.empty()) {
                    if (!methods.empty() && !list_contains(split_name_list(methods), "password")) {
                        std::cerr << "authentication failed: password method not offered by server";
                        std::cerr << " (methods: " << methods << ')' << '\n';
                        return false;
                    }

                    SshUserauthRequestMessage auth_req;
                    auth_req.username = args.user;
                    auth_req.service_name = SSH_SERVICE_CONNECTION;
                    auth_req.method_name = "password";
                    auth_req.method_specific_data = make_password_method_data(args.password);
                    if (!send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req))) {
                        std::cerr << "failed to send password auth fallback request\n";
                        return false;
                    }
                    attempted_publickey = false;
                    waiting_pk_ok = false;
                    debug("publickey failed; sent password fallback request");
                    continue;
                }

                std::cerr << "authentication failed";
                if (!methods.empty()) {
                    std::cerr << " (methods: " << methods << ')';
                }
                std::cerr << '\n';
                return false;
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_PK_OK) {
                if (!waiting_pk_ok || publickey_blob.empty()) {
                    std::cerr << "unexpected USERAUTH_PK_OK\n";
                    return false;
                }

                auto pk_ok = SshMessageCodec::decode_userauth_pk_ok(payload.data(), payload.size());
                if (!pk_ok) {
                    std::cerr << "invalid USERAUTH_PK_OK payload\n";
                    return false;
                }
                if (pk_ok->algorithm_name != publickey_algorithm || pk_ok->public_key_blob != publickey_blob) {
                    std::cerr << "USERAUTH_PK_OK key mismatch\n";
                    return false;
                }

                auto maybe_signature = make_publickey_signature(
                    transport.session_id(),
                    args.user,
                    publickey_algorithm,
                    publickey_blob,
                    private_key_der,
                    crypto);
                if (!maybe_signature) {
                    std::cerr << "failed to sign publickey auth request\n";
                    return false;
                }

                SshUserauthRequestMessage auth_req;
                auth_req.username = args.user;
                auth_req.service_name = SSH_SERVICE_CONNECTION;
                auth_req.method_name = "publickey";
                auth_req.method_specific_data = make_publickey_method_data(
                    publickey_algorithm,
                    publickey_blob,
                    true,
                    *maybe_signature);
                if (!send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req))) {
                    std::cerr << "failed to send signed publickey auth request\n";
                    return false;
                }

                waiting_pk_ok = false;
                debug("sent signed publickey auth request");
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
                auto conf = SshMessageCodec::decode_channel_open_confirmation(payload.data(), payload.size());
                if (!conf) {
                    std::cerr << "invalid channel confirmation\n";
                    return false;
                }

                auto pending_it = pending_local_open_socket.find(conf->recipient_channel);
                if (pending_it != pending_local_open_socket.end()) {
                    forward_local_to_socket[conf->recipient_channel] = pending_it->second;
                    forward_local_to_remote[conf->recipient_channel] = conf->sender_channel;
                    forward_remote_to_local[conf->sender_channel] = conf->recipient_channel;
                    debug("local-forward open confirmed local=" + std::to_string(conf->recipient_channel) +
                          " remote=" + std::to_string(conf->sender_channel));
                    pending_local_open_socket.erase(pending_it);
                    continue;
                }

                auto pending_dynamic_it = pending_dynamic_open_socket.find(conf->recipient_channel);
                if (pending_dynamic_it != pending_dynamic_open_socket.end()) {
                    forward_local_to_socket[conf->recipient_channel] = pending_dynamic_it->second;
                    forward_local_to_remote[conf->recipient_channel] = conf->sender_channel;
                    forward_remote_to_local[conf->sender_channel] = conf->recipient_channel;
                    debug("dynamic-forward open confirmed local=" + std::to_string(conf->recipient_channel) +
                          " remote=" + std::to_string(conf->sender_channel));
                    (void)send_socks_reply(pending_dynamic_it->second, 0x00);
                    pending_dynamic_open_socket.erase(pending_dynamic_it);
                    continue;
                }

                if (conf->recipient_channel != local_channel_id) {
                    continue;
                }

                remote_channel_id = conf->sender_channel;
                channel_opened = true;

                if (!interactive_mode) {
                    SshChannelRequestMessage req;
                    req.recipient_channel = remote_channel_id;
                    req.request_type = "exec";
                    req.want_reply = true;
                    req.request_specific_data = make_exec_request_data(args.command);
                    if (!send_packet(fd, transport, SshMessageCodec::encode_channel_request(req))) {
                        std::cerr << "failed to send exec request\n";
                        return false;
                    }
                    exec_sent = true;
                    debug("sent exec request");
                } else {
                    SshChannelRequestMessage pty_req;
                    pty_req.recipient_channel = remote_channel_id;
                    pty_req.request_type = "pty-req";
                    pty_req.want_reply = true;
                    pty_req.request_specific_data = make_pty_request_data();
                    if (!send_packet(fd, transport, SshMessageCodec::encode_channel_request(pty_req))) {
                        std::cerr << "failed to send pty request\n";
                        return false;
                    }
                    pty_sent = true;
                    debug("sent pty request");
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_OPEN_FAILURE) {
                auto open_failure = SshMessageCodec::decode_channel_open_failure(payload.data(), payload.size());
                if (!open_failure) {
                    continue;
                }

                auto pending_it = pending_local_open_socket.find(open_failure->recipient_channel);
                if (pending_it != pending_local_open_socket.end()) {
                    close_socket_handle(pending_it->second);
                    pending_local_open_socket.erase(pending_it);
                    continue;
                }

                auto pending_dynamic_it = pending_dynamic_open_socket.find(open_failure->recipient_channel);
                if (pending_dynamic_it != pending_dynamic_open_socket.end()) {
                    debug("dynamic-forward open failed local=" + std::to_string(open_failure->recipient_channel) +
                          " reason=" + std::to_string(open_failure->reason_code));
                    (void)send_socks_reply(pending_dynamic_it->second, 0x05);
                    close_socket_handle(pending_dynamic_it->second);
                    pending_dynamic_open_socket.erase(pending_dynamic_it);
                    continue;
                }

                if (open_failure->recipient_channel == local_channel_id) {
                    std::cerr << "failed to open session channel: " << open_failure->description << '\n';
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_REQUEST_SUCCESS) {
                if (!pending_remote_forward_specs.empty()) {
                    remote_forward_specs.push_back(pending_remote_forward_specs.front());
                    pending_remote_forward_specs.erase(pending_remote_forward_specs.begin());
                }
            } else if (type == SshMessageType::SSH_MSG_REQUEST_FAILURE) {
                if (!pending_remote_forward_specs.empty()) {
                    const auto failed = pending_remote_forward_specs.front();
                    std::cerr << "remote forward rejected: "
                              << failed.bind_addr << ':' << failed.bind_port
                              << " -> " << failed.target_host << ':' << failed.target_port << '\n';
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_OPEN) {
                auto open = SshMessageCodec::decode_channel_open(payload.data(), payload.size());
                if (!open) {
                    continue;
                }
                if (open->channel_type != SSH_CHANNEL_FORWARDED_TCPIP) {
                    continue;
                }

                size_t offset = 0;
                auto connected_address = SshMessageCodec::read_string(
                    open->type_specific_data.data(),
                    open->type_specific_data.size(),
                    offset);
                if (!connected_address || offset + 4 > open->type_specific_data.size()) {
                    continue;
                }
                const uint32_t connected_port = SshMessageCodec::read_uint32(
                    open->type_specific_data.data(),
                    open->type_specific_data.size(),
                    offset);
                auto originator_address = SshMessageCodec::read_string(
                    open->type_specific_data.data(),
                    open->type_specific_data.size(),
                    offset);
                if (!originator_address || offset + 4 > open->type_specific_data.size()) {
                    continue;
                }
                const uint32_t originator_port = SshMessageCodec::read_uint32(
                    open->type_specific_data.data(),
                    open->type_specific_data.size(),
                    offset);

                std::optional<RemoteForwardSpec> matched_spec;
                for (const auto &spec : remote_forward_specs) {
                    if (spec.bind_addr == *connected_address && spec.bind_port == static_cast<uint16_t>(connected_port)) {
                        matched_spec = spec;
                        break;
                    }
                }
                if (!matched_spec) {
                    SshChannelOpenFailureMessage fail;
                    fail.recipient_channel = open->sender_channel;
                    fail.reason_code = static_cast<uint32_t>(SshChannelOpenFailureReason::SSH_OPEN_CONNECT_FAILED);
                    fail.description = "No matching -R forwarding target";
                    fail.language = "en";
                    (void)send_packet(fd, transport, SshMessageCodec::encode_channel_open_failure(fail));
                    continue;
                }

                SocketGuard forward_sock;
                if (!connect_tcp(matched_spec->target_host, matched_spec->target_port, forward_sock)) {
                    SshChannelOpenFailureMessage fail;
                    fail.recipient_channel = open->sender_channel;
                    fail.reason_code = static_cast<uint32_t>(SshChannelOpenFailureReason::SSH_OPEN_CONNECT_FAILED);
                    fail.description = "Remote forward target connect failed";
                    fail.language = "en";
                    (void)send_packet(fd, transport, SshMessageCodec::encode_channel_open_failure(fail));
                    continue;
                }

                SshChannelOpenConfirmationMessage conf;
                conf.recipient_channel = open->sender_channel;
                const uint32_t local_forward_id = next_forward_local_channel_id++;
                conf.sender_channel = local_forward_id;
                conf.initial_window_size = open->initial_window_size;
                conf.maximum_packet_size = open->maximum_packet_size;
                (void)send_packet(fd, transport, SshMessageCodec::encode_channel_open_confirmation(conf));

                forward_local_to_remote[local_forward_id] = open->sender_channel;
                forward_remote_to_local[open->sender_channel] = local_forward_id;
                forward_local_to_socket[local_forward_id] = forward_sock.fd;
                forward_sock.fd = kInvalidSocket;
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_DATA) {
                auto data_msg = SshMessageCodec::decode_channel_data(payload.data(), payload.size());
                if (data_msg && !data_msg->data.empty()) {
                auto forward_it = forward_local_to_socket.find(data_msg->recipient_channel);
                if (forward_it != forward_local_to_socket.end() && forward_it->second != kInvalidSocket) {
                    debug("local-forward recv data bytes=" + std::to_string(data_msg->data.size()) +
                          " local=" + std::to_string(data_msg->recipient_channel));
                    if (!send_all(forward_it->second, data_msg->data.data(), data_msg->data.size())) {
                        auto remote_it = forward_local_to_remote.find(data_msg->recipient_channel);
                        if (remote_it != forward_local_to_remote.end()) {
                            SshChannelCloseMessage close_msg;
                            close_msg.recipient_channel = remote_it->second;
                            (void)send_packet(fd, transport, SshMessageCodec::encode_channel_close(close_msg));
                        }
                        close_forward_channel(data_msg->recipient_channel);
                    }
                } else {
                        std::cout.write(reinterpret_cast<const char *>(data_msg->data.data()),
                                        static_cast<std::streamsize>(data_msg->data.size()));
                        std::cout.flush();
                    }
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_EXTENDED_DATA) {
                auto ext_msg = SshMessageCodec::decode_channel_extended_data(payload.data(), payload.size());
                if (ext_msg && !ext_msg->data.empty()) {
                    write_stderr(ext_msg->data);
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                continue;
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_EOF) {
                auto eof_msg = SshMessageCodec::decode_channel_eof(payload.data(), payload.size());
                if (eof_msg) {
                    auto forward_it = forward_local_to_socket.find(eof_msg->recipient_channel);
                    if (forward_it != forward_local_to_socket.end()) {
                        shutdown_socket_write(forward_it->second);
                    }
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_REQUEST) {
                auto req = SshMessageCodec::decode_channel_request(payload.data(), payload.size());
                if (req && req->request_type == "exit-status") {
                    size_t offset = 0;
                    const uint32_t status = SshMessageCodec::read_uint32(
                        req->request_specific_data.data(), req->request_specific_data.size(), offset);
                    exit_code = status;
                    got_exit_status = true;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_SUCCESS) {
                auto success = SshMessageCodec::decode_channel_success(payload.data(), payload.size());
                if (!success) {
                    continue;
                }
                if (interactive_mode && pty_sent && !pty_ok) {
                    pty_ok = true;

                    SshChannelRequestMessage shell_req;
                    shell_req.recipient_channel = remote_channel_id;
                    shell_req.request_type = "shell";
                    shell_req.want_reply = true;
                    if (!send_packet(fd, transport, SshMessageCodec::encode_channel_request(shell_req))) {
                        std::cerr << "failed to send shell request\n";
                        return false;
                    }
                    shell_sent = true;
                    debug("sent shell request");
                } else if (interactive_mode && shell_sent) {
                    shell_ready = true;
                    debug("interactive shell ready");
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_FAILURE) {
                if (interactive_mode && pty_sent && !pty_ok) {
                    std::cerr << "pty request failed\n";
                    return false;
                }
                if (interactive_mode && shell_sent) {
                    std::cerr << "shell request failed\n";
                    return false;
                }
                if (exec_sent) {
                    std::cerr << "exec request failed\n";
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_CLOSE) {
                auto close_msg = SshMessageCodec::decode_channel_close(payload.data(), payload.size());
                if (close_msg) {
                    SshChannelCloseMessage back;
                    back.recipient_channel = close_msg->recipient_channel;
                    (void)send_packet(fd, transport, SshMessageCodec::encode_channel_close(back));

                    if (close_msg->recipient_channel != local_channel_id) {
                        close_forward_channel(close_msg->recipient_channel);
                        continue;
                    }
                }

                for (const auto &spec : remote_forward_specs) {
                    SshGlobalRequestMessage req;
                    req.request_name = "cancel-tcpip-forward";
                    req.want_reply = false;
                    yuan::buffer::ByteBuffer data;
                    SshMessageCodec::write_string(data, spec.bind_addr);
                    SshMessageCodec::write_uint32(data, spec.bind_port);
                    req.request_specific_data.assign(
                        reinterpret_cast<const uint8_t *>(data.read_ptr()),
                        reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes());
                    (void)send_packet(fd, transport, SshMessageCodec::encode_global_request(req));
                }
                cleanup_forward_resources();
                if (got_exit_status) {
                    return exit_code == 0;
                }
                return true;
            } else if (type == SshMessageType::SSH_MSG_DISCONNECT) {
                auto disc = SshMessageCodec::decode_disconnect(payload.data(), payload.size());
                std::cerr << "disconnected";
                if (disc) {
                    std::cerr << ": " << disc->description;
                }
                std::cerr << '\n';
                return false;
            }

            if (interactive_mode && shell_ready) {
#ifndef _WIN32
                if (g_cli_sigint_count > handled_sigint_count) {
                    handled_sigint_count = g_cli_sigint_count;
                    SshChannelRequestMessage sig_req;
                    sig_req.recipient_channel = remote_channel_id;
                    sig_req.request_type = "signal";
                    sig_req.want_reply = false;
                    sig_req.request_specific_data = make_signal_request_data("INT");
                    (void)send_packet(fd, transport, SshMessageCodec::encode_channel_request(sig_req));
                }

                TerminalSize current_size = query_terminal_size();
                if (current_size.cols != last_terminal_size.cols ||
                    current_size.rows != last_terminal_size.rows ||
                    current_size.pixel_width != last_terminal_size.pixel_width ||
                    current_size.pixel_height != last_terminal_size.pixel_height) {
                    last_terminal_size = current_size;
                    SshChannelRequestMessage wc_req;
                    wc_req.recipient_channel = remote_channel_id;
                    wc_req.request_type = "window-change";
                    wc_req.want_reply = false;
                    wc_req.request_specific_data = make_window_change_request_data(
                        current_size.cols,
                        current_size.rows,
                        current_size.pixel_width,
                        current_size.pixel_height);
                    (void)send_packet(fd, transport, SshMessageCodec::encode_channel_request(wc_req));
                }
#endif
            }

            if (!pump_forward_target_reads()) {
                std::cerr << "failed to send forwarded data\n";
                return false;
            }
            if (!pump_local_forward_accepts()) {
                std::cerr << "failed to accept local forward clients\n";
                return false;
            }
            if (!pump_dynamic_forward_accepts() || !pump_dynamic_socks_handshake()) {
                std::cerr << "failed to process dynamic forwards\n";
                return false;
            }

            (void)service_accepted;
            (void)auth_ok;
            (void)channel_opened;
        }
    }
}

int main(int argc, char **argv)
{
    yuan::platform::NativePlatformGuard native_guard;
    if (!native_guard.ok()) {
        std::cerr << "native platform init failed\n";
        return 1;
    }

#ifndef _WIN32
    signal(SIGINT, cli_sigint_handler);
#endif

    CliArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage(argv[0]);
        return 2;
    }
    if (args.help) {
        print_usage(argv[0]);
        return 0;
    }
    if (args.version) {
        std::cout << "release_ssh_cli 1.0\n";
        return 0;
    }

    SocketGuard sock;
    if (!connect_tcp(args.host, static_cast<uint16_t>(args.port), sock)) {
        std::cerr << "connect failed: " << args.host << ':' << args.port << '\n';
        return 1;
    }
    (void)set_recv_timeout(sock.fd, args.timeout_ms);

    const bool ok = args.probe ? run_version_probe(sock.fd, args) : run_exec_phase1(sock.fd, args);
    return ok ? 0 : 1;
}
