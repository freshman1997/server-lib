#include "net/iocp/iocp_accept.h"
#include "net/iocp/iocp_completion_port.h"
#include "net/iocp/iocp_tcp_io.h"
#include "net/socket/socket_ops.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

#ifdef _WIN32
    bool test_accept_ex_round_trip()
    {
        yuan::net::IocpCompletionPort port;
        if (!require(port.init(), "accept iocp init should succeed")) {
            return false;
        }

        const int listener = yuan::net::socket::create_ipv4_overlapped_tcp_socket(false);
        if (!require(listener >= 0, "overlapped listener socket should be created")) {
            return false;
        }
        yuan::net::socket::set_reuse_addr(listener, true);
        if (!require(yuan::net::socket::bind(listener, yuan::net::InetAddress("127.0.0.1", 0)) == 0,
                     "listener bind should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::socket::listen(listener, 16) == 0, "listener listen should succeed")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(port.associate_socket(listener, 0xfeed), "listener should associate with iocp")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpAcceptEx accept_ex;
        if (!require(accept_ex.load(listener), "AcceptEx functions should load")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        const int accepted = yuan::net::socket::create_ipv4_overlapped_tcp_socket(false);
        if (!require(accepted >= 0, "overlapped accepted socket should be created")) {
            yuan::net::socket::close_fd(listener);
            return false;
        }

        OVERLAPPED overlapped{};
        std::array<char, yuan::net::kIocpAcceptBufferBytes> address_buffer{};
        if (!require(accept_ex.post(listener, accepted, address_buffer.data(), address_buffer.size(), &overlapped),
                     "AcceptEx post should succeed or pend")) {
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        const auto listen_addr = yuan::net::socket::get_local_address(listener);
        const int client = yuan::net::socket::create_ipv4_tcp_socket(false);
        if (!require(client >= 0, "client socket should be created")) {
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::socket::connect(client, listen_addr) == 0, "client connect should succeed")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpCompletion completion;
        if (!require(port.wait(1000, completion), "AcceptEx completion should arrive")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.ok, "AcceptEx completion should be ok")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.operation == &overlapped, "AcceptEx operation should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(accept_ex.update_accept_context(accepted, listener),
                     "accepted socket context should update")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::IocpAcceptedAddresses addresses;
        if (!require(accept_ex.parse_addresses(address_buffer.data(), address_buffer.size(), addresses),
                     "AcceptEx addresses should parse")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(port.associate_socket(accepted, 0xbeef), "accepted socket should associate with iocp")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        OVERLAPPED recv_overlapped{};
        std::array<char, 16> recv_buffer{};
        if (!require(yuan::net::IocpTcpIo::post_recv(accepted,
                                                      recv_buffer.data(),
                                                      static_cast<uint32_t>(recv_buffer.size()),
                                                      &recv_overlapped),
                     "overlapped recv should post")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        constexpr char kPayload[] = "ping";
        if (!require(::send(static_cast<SOCKET>(client), kPayload, 4, 0) == 4,
                     "client send should succeed")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(port.wait(1000, completion), "recv completion should arrive")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.ok, "recv completion should be ok")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.operation == &recv_overlapped, "recv operation should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.bytes == 4 && std::memcmp(recv_buffer.data(), kPayload, 4) == 0,
                     "recv payload should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        OVERLAPPED send_overlapped{};
        constexpr char kReply[] = "pong";
        if (!require(yuan::net::IocpTcpIo::post_send(accepted, kReply, 4, &send_overlapped),
                     "overlapped send should post")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(port.wait(1000, completion), "send completion should arrive")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.ok, "send completion should be ok")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(completion.operation == &send_overlapped, "send operation should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        std::array<char, 16> reply_buffer{};
        if (!require(::recv(static_cast<SOCKET>(client), reply_buffer.data(), 4, 0) == 4,
                     "client recv should succeed")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(std::memcmp(reply_buffer.data(), kReply, 4) == 0,
                     "send payload should round-trip")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }
        if (!require(yuan::net::IocpTcpIo::cancel(accepted),
                     "cancel with no outstanding accepted socket IO should be harmless")) {
            yuan::net::socket::close_fd(client);
            yuan::net::socket::close_fd(accepted);
            yuan::net::socket::close_fd(listener);
            return false;
        }

        yuan::net::socket::close_fd(client);
        yuan::net::socket::close_fd(accepted);
        yuan::net::socket::close_fd(listener);
        return true;
    }
#endif
}

int main()
{
    yuan::net::IocpCompletionPort port;

#ifdef _WIN32
    if (!require(port.init(), "iocp init should succeed on Windows")) {
        return 1;
    }
    if (!require(port.valid(), "iocp should be valid after init")) {
        return 1;
    }

    int marker = 0;
    constexpr uintptr_t kKey = 0x1234;
    constexpr uint32_t kBytes = 17;
    if (!require(port.post(kKey, &marker, kBytes), "iocp post should succeed")) {
        return 1;
    }

    yuan::net::IocpCompletion completion;
    if (!require(port.wait(1000, completion), "iocp wait should receive posted completion")) {
        return 1;
    }
    if (!require(completion.ok, "posted completion should be ok")) {
        return 1;
    }
    if (!require(completion.key == kKey, "posted completion key should round-trip")) {
        return 1;
    }
    if (!require(completion.bytes == kBytes, "posted completion byte count should round-trip")) {
        return 1;
    }
    if (!require(completion.operation == &marker, "posted completion operation should round-trip")) {
        return 1;
    }

    port.close();
    if (!require(!port.valid(), "iocp should be invalid after close")) {
        return 1;
    }
    if (!test_accept_ex_round_trip()) {
        return 1;
    }
#else
    if (!require(!port.init(), "iocp init should be unavailable off Windows")) {
        return 1;
    }
    if (!require(!port.valid(), "iocp should remain invalid off Windows")) {
        return 1;
    }
#endif

    return 0;
}
