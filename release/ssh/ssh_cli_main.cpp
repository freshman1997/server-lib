#include "algorithm/ssh_algorithm_registry.h"
#include "algorithm/ssh_host_key_algorithm.h"
#include "auth/ssh_auth_publickey.h"
#include "crypto/ssh_crypto_openssl.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_config.h"
#include "transport/ssh_transport.h"
#include "transport/ssh_version_exchange.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
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

    struct CliArgs
    {
        std::string host = "127.0.0.1";
        int port = 2222;
        int timeout_ms = 5000;
        std::string user;
        std::string password;
        std::string command;
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

    void print_usage(const char *program)
    {
        std::cout
            << "release_ssh_cli (phase-1 exec)\n"
            << "usage:\n"
            << "  " << program << " --host <host> --port <port> --user <user> --password <password> --command <cmd>\n"
            << "  " << program << " [host] [port] [timeout_ms] --user <user> --password <password> --command <cmd>\n\n"
            << "options:\n"
            << "  --host <host>\n"
            << "  --port <port>\n"
            << "  --timeout-ms <ms>\n"
            << "  --user <user>\n"
            << "  --password <password>\n"
            << "  --command <cmd>\n"
            << "  -h, --help\n\n"
            << "env defaults:\n"
            << "  YUAN_SSH_HOST\n"
            << "  YUAN_SSH_PORT\n"
            << "  YUAN_SSH_TIMEOUT_MS\n"
            << "  YUAN_SSH_USER\n"
            << "  YUAN_SSH_PASSWORD\n";
    }

    bool parse_args(int argc, char **argv, CliArgs &args)
    {
        args.host = read_env_string("YUAN_SSH_HOST", args.host);
        args.port = read_env_int("YUAN_SSH_PORT", args.port);
        args.timeout_ms = read_env_int("YUAN_SSH_TIMEOUT_MS", args.timeout_ms);
        args.user = read_env_string("YUAN_SSH_USER", args.user);
        args.password = read_env_string("YUAN_SSH_PASSWORD", args.password);

        int idx = 1;
        if (idx < argc && argv[idx][0] != '-') {
            args.host = argv[idx++];
        }
        if (idx < argc && argv[idx][0] != '-') {
            try {
                args.port = std::stoi(argv[idx++]);
            } catch (...) {
                std::cerr << "invalid port\n";
                return false;
            }
        }
        if (idx < argc && argv[idx][0] != '-') {
            try {
                args.timeout_ms = std::stoi(argv[idx++]);
            } catch (...) {
                std::cerr << "invalid timeout_ms\n";
                return false;
            }
        }

        while (idx < argc) {
            const std::string opt = argv[idx++];
            if (opt == "-h" || opt == "--help") {
                print_usage(argv[0]);
                return false;
            }

            if (idx >= argc) {
                std::cerr << "missing value for " << opt << '\n';
                return false;
            }
            const std::string value = argv[idx++];

            if (opt == "--host") {
                args.host = value;
            } else if (opt == "--port") {
                try {
                    args.port = std::stoi(value);
                } catch (...) {
                    std::cerr << "invalid --port\n";
                    return false;
                }
            } else if (opt == "--timeout-ms") {
                try {
                    args.timeout_ms = std::stoi(value);
                } catch (...) {
                    std::cerr << "invalid --timeout-ms\n";
                    return false;
                }
            } else if (opt == "--user") {
                args.user = value;
            } else if (opt == "--password") {
                args.password = value;
            } else if (opt == "--command") {
                args.command = value;
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
        if (args.user.empty()) {
            std::cerr << "--user is required\n";
            return false;
        }
        if (args.password.empty()) {
            std::cerr << "--password is required\n";
            return false;
        }
        if (args.command.empty()) {
            std::cerr << "--command is required\n";
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

    bool recv_some(SocketHandle fd, std::vector<uint8_t> &out)
    {
        std::array<uint8_t, 64 * 1024> buf{};
#ifdef _WIN32
        const int n = recv(fd, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0);
#else
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
#endif
        if (n <= 0) {
            return false;
        }
        out.assign(buf.begin(), buf.begin() + n);
        return true;
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

    bool read_packet(SocketHandle fd,
                     yuan::buffer::ByteBuffer &recv_buf,
                     yuan::net::ssh::SshTransport &transport,
                     std::vector<uint8_t> &payload)
    {
        for (;;) {
            auto parse = transport.try_parse_packet(recv_buf);
            if (parse.invalid) {
                return false;
            }
            if (!parse.complete) {
                std::vector<uint8_t> chunk;
                if (!recv_some(fd, chunk)) {
                    return false;
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
                return false;
            }
            payload = std::move(*decoded);
            return true;
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

    std::vector<uint8_t> make_exec_request_data(const std::string &command)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_string(data, command);
        return {
            reinterpret_cast<const uint8_t *>(data.read_ptr()),
            reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes()
        };
    }

    bool run_exec_phase1(SocketHandle fd, const CliArgs &args)
    {
        using namespace yuan::net::ssh;

        SshCryptoOpenSSL crypto;
        SshAlgorithmRegistry registry;
        registry.register_defaults();

        SshTransport transport(&registry, &crypto, false);
        transport.set_we_are_server(false);

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

        yuan::buffer::ByteBuffer recv_buf;
        bool got_peer_kexinit = false;
        bool sent_kex_init = false;
        bool sent_newkeys = false;
        bool got_newkeys = false;

        while (!got_newkeys) {
            std::vector<uint8_t> payload;
            if (!read_packet(fd, recv_buf, transport, payload)) {
                std::cerr << "failed while reading kex packets\n";
                return false;
            }
            if (payload.empty()) {
                continue;
            }

            const auto msg_type = static_cast<SshMessageType>(payload[0]);
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

                if (!send_packet(fd, transport, SshMessageCodec::encode_newkeys())) {
                    std::cerr << "failed to send NEWKEYS\n";
                    return false;
                }
                sent_newkeys = true;
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
            }
        }

        SshServiceRequestMessage service_req;
        service_req.service_name = SSH_SERVICE_USERAUTH;
        if (!send_packet(fd, transport, SshMessageCodec::encode_service_request(service_req))) {
            std::cerr << "failed to send service request\n";
            return false;
        }

        bool service_accepted = false;
        bool auth_ok = false;
        bool channel_opened = false;
        bool exec_sent = false;
        uint32_t local_channel_id = 0;
        uint32_t remote_channel_id = 0;
        uint32_t exit_code = 0;
        bool got_exit_status = false;

        while (true) {
            std::vector<uint8_t> payload;
            if (!read_packet(fd, recv_buf, transport, payload)) {
                std::cerr << "connection closed\n";
                return false;
            }
            if (payload.empty()) {
                continue;
            }

            const auto type = static_cast<SshMessageType>(payload[0]);
            if (type == SshMessageType::SSH_MSG_SERVICE_ACCEPT) {
                service_accepted = true;

                SshUserauthRequestMessage auth_req;
                auth_req.username = args.user;
                auth_req.service_name = SSH_SERVICE_CONNECTION;
                auth_req.method_name = "password";
                auth_req.method_specific_data = make_password_method_data(args.password);
                if (!send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req))) {
                    std::cerr << "failed to send userauth request\n";
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_SUCCESS) {
                auth_ok = true;

                SshChannelOpenMessage open_msg;
                open_msg.channel_type = SSH_CHANNEL_SESSION;
                open_msg.sender_channel = local_channel_id;
                open_msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
                open_msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;
                if (!send_packet(fd, transport, SshMessageCodec::encode_channel_open(open_msg))) {
                    std::cerr << "failed to open channel\n";
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_FAILURE) {
                auto fail = SshMessageCodec::decode_userauth_failure(payload.data(), payload.size());
                std::cerr << "authentication failed";
                if (fail) {
                    std::cerr << " (methods: " << fail->auth_methods_that_can_continue << ')';
                }
                std::cerr << '\n';
                return false;
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
                auto conf = SshMessageCodec::decode_channel_open_confirmation(payload.data(), payload.size());
                if (!conf) {
                    std::cerr << "invalid channel confirmation\n";
                    return false;
                }
                remote_channel_id = conf->sender_channel;
                channel_opened = true;

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
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_DATA) {
                auto data_msg = SshMessageCodec::decode_channel_data(payload.data(), payload.size());
                if (data_msg && !data_msg->data.empty()) {
                    std::cout.write(reinterpret_cast<const char *>(data_msg->data.data()),
                                    static_cast<std::streamsize>(data_msg->data.size()));
                    std::cout.flush();
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_EXTENDED_DATA) {
                auto ext_msg = SshMessageCodec::decode_channel_extended_data(payload.data(), payload.size());
                if (ext_msg && !ext_msg->data.empty()) {
                    std::cerr.write(reinterpret_cast<const char *>(ext_msg->data.data()),
                                    static_cast<std::streamsize>(ext_msg->data.size()));
                    std::cerr.flush();
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
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_CLOSE) {
                auto close_msg = SshMessageCodec::decode_channel_close(payload.data(), payload.size());
                if (close_msg) {
                    SshChannelCloseMessage back;
                    back.recipient_channel = close_msg->recipient_channel;
                    (void)send_packet(fd, transport, SshMessageCodec::encode_channel_close(back));
                }
                if (got_exit_status) {
                    return exit_code == 0;
                }
                return true;
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_FAILURE) {
                if (exec_sent) {
                    std::cerr << "exec request failed\n";
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_DISCONNECT) {
                auto disc = SshMessageCodec::decode_disconnect(payload.data(), payload.size());
                std::cerr << "disconnected";
                if (disc) {
                    std::cerr << ": " << disc->description;
                }
                std::cerr << '\n';
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
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    CliArgs args;
    if (!parse_args(argc, argv, args)) {
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    SocketGuard sock;
    if (!connect_tcp(args.host, static_cast<uint16_t>(args.port), sock)) {
        std::cerr << "connect failed: " << args.host << ':' << args.port << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    (void)set_recv_timeout(sock.fd, args.timeout_ms);

    const bool ok = run_exec_phase1(sock.fd, args);

#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
