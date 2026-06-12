#ifndef YUAN_RELEASE_SSH_CLI_TRANSPORT_H
#define YUAN_RELEASE_SSH_CLI_TRANSPORT_H

#include "ssh_cli_config.h"
#include "ssh_cli_socket.h"

#include "algorithm/ssh_algorithm_registry.h"
#include "crypto/ssh_crypto_openssl.h"
#include "transport/ssh_transport.h"
#include "buffer/byte_buffer.h"

#include <functional>
#include <string>

namespace yuan::release_ssh::client
{
    bool run_version_probe(SocketHandle fd, const CliArgs &args);

    bool perform_client_key_exchange(SocketHandle fd,
                                     const CliArgs &args,
                                     yuan::net::ssh::SshTransport &transport,
                                     yuan::net::ssh::SshAlgorithmRegistry &registry,
                                     yuan::net::ssh::SshCryptoOpenSSL &crypto,
                                     yuan::buffer::ByteBuffer &recv_buf,
                                     const std::function<void(const std::string &)> &debug);
}

#endif
