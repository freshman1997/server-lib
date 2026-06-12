#ifndef YUAN_RELEASE_SSH_CLI_AUTH_H
#define YUAN_RELEASE_SSH_CLI_AUTH_H

#include "crypto/ssh_crypto_openssl.h"
#include "ssh_cli_config.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::release_ssh::client
{
    struct ClientIdentity
    {
        std::string algorithm;
        std::vector<uint8_t> public_key_blob;
        std::vector<uint8_t> private_key_der;
    };

    bool check_known_hosts(const CliArgs &args,
                           const std::vector<uint8_t> &host_key_blob,
                           std::string &error_message);
    std::vector<std::string> split_name_list(const std::string &csv);
    bool list_contains(const std::vector<std::string> &names, std::string_view needle);
    std::optional<std::vector<uint8_t>> load_private_key_der_any(const std::string &path);
    std::optional<std::vector<uint8_t>> build_public_key_blob_from_private_der(const std::vector<uint8_t> &private_key_der,
                                                                                std::string &algorithm_out);
    bool load_client_identity(const CliArgs &args, ClientIdentity &identity, std::ostream &err);
    std::vector<uint8_t> make_password_method_data(const std::string &password);
    std::vector<uint8_t> make_publickey_method_data(const std::string &algorithm,
                                                    const std::vector<uint8_t> &public_key_blob,
                                                    bool include_signature,
                                                    const std::vector<uint8_t> &signature);
    std::optional<std::vector<uint8_t>> make_publickey_signature(const std::vector<uint8_t> &session_id,
                                                                 const std::string &username,
                                                                 const std::string &algorithm,
                                                                 const std::vector<uint8_t> &public_key_blob,
                                                                 const std::vector<uint8_t> &private_key_der,
                                                                 yuan::net::ssh::SshCryptoOpenSSL &crypto);
}

#endif
