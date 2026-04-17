#ifndef __NET_SSH_SSH_CONFIG_H__
#define __NET_SSH_SSH_CONFIG_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    struct SshServerConfig
    {
        uint16_t port = 22;
        std::vector<std::string> host_key_paths;
        std::vector<std::string> host_key_algorithms = {
            "ssh-ed25519",
            "ecdsa-sha2-nistp256",
            "ecdsa-sha2-nistp384",
            "ecdsa-sha2-nistp521",
            "rsa-sha2-512",
            "rsa-sha2-256"
        };
        std::vector<std::string> kex_algorithms = {
            "curve25519-sha256",
            "curve25519-sha256@libssh.org",
            "ecdh-sha2-nistp256",
            "ecdh-sha2-nistp384",
            "ecdh-sha2-nistp521",
            "diffie-hellman-group14-sha256",
            "diffie-hellman-group16-sha512",
            "diffie-hellman-group18-sha512"
        };
        std::vector<std::string> cipher_algorithms = {
            "chacha20-poly1305@openssh.com",
            "aes256-gcm@openssh.com",
            "aes128-gcm@openssh.com",
            "aes256-ctr",
            "aes192-ctr",
            "aes128-ctr"
        };
        std::vector<std::string> mac_algorithms = {
            "hmac-sha2-256",
            "hmac-sha2-512",
            "hmac-sha1"
        };
        std::vector<std::string> compression_algorithms = {
            "none",
            "zlib@openssh.com",
            "zlib"
        };
        std::vector<std::string> auth_methods = {
            "publickey",
            "password",
            "keyboard-interactive"
        };
        uint32_t max_sessions = 1000;
        uint32_t max_channels_per_session = 64;
        uint32_t idle_timeout_ms = 0;
        uint32_t max_auth_attempts = 6;
        uint32_t auth_timeout_ms = 60000;
        std::string banner;
        bool enable_port_forwarding = true;
        bool enable_sftp = true;
        std::string sftp_root_dir;
        std::string software_version = "YuanSSH_1.0";
    };
}

#endif
