#include "ssh_cli_transport.h"

#include "ssh_cli_auth.h"

#include "algorithm/ssh_host_key_algorithm.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_config.h"
#include "transport/ssh_version_exchange.h"

#include <iostream>
#include <utility>
#include <vector>

namespace yuan::release_ssh::client
{
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

    bool perform_client_key_exchange(SocketHandle fd,
                                     const CliArgs &args,
                                     yuan::net::ssh::SshTransport &transport,
                                     yuan::net::ssh::SshAlgorithmRegistry &registry,
                                     yuan::net::ssh::SshCryptoOpenSSL &crypto,
                                     yuan::buffer::ByteBuffer &recv_buf,
                                     const std::function<void(const std::string &)> &debug)
    {
        using namespace yuan::net::ssh;

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

        return true;
    }
}
