#include "net/socket/inet_address.h"
#include "net/socket/socket_ops.h"
#include "net/socket/socket.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace yuan::net;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                           \
    do {                                     \
        std::cout << "  " << name << "... "; \
    } while (0)

#define PASS()                          \
    do {                                \
        std::cout << "OK" << std::endl; \
        ++tests_passed;                 \
    } while (0)

#define FAIL(msg)                                    \
    do {                                             \
        std::cout << "FAILED: " << msg << std::endl; \
        ++tests_failed;                              \
    } while (0)

#define ASSERT_EQ(a, b)         \
    do {                        \
        if ((a) != (b)) {       \
            FAIL(#a " != " #b); \
            return;             \
        }                       \
    } while (0)

#define ASSERT_TRUE(cond) \
    do {                  \
        if (!(cond)) {    \
            FAIL(#cond);  \
            return;       \
        }                 \
    } while (0)

static void test_inet_address_ipv4_default()
{
    TEST("InetAddress default constructor (IPv4)");
    InetAddress addr;
    ASSERT_EQ(addr.family(), AddressFamily::ipv4);
    ASSERT_TRUE(!addr.is_ipv6());
    ASSERT_EQ(addr.get_port(), 0);
    PASS();
}

static void test_inet_address_ipv4_explicit()
{
    TEST("InetAddress IPv4 explicit");
    InetAddress addr("127.0.0.1", 8080);
    ASSERT_EQ(addr.family(), AddressFamily::ipv4);
    ASSERT_TRUE(!addr.is_ipv6());
    ASSERT_EQ(addr.get_ip(), "127.0.0.1");
    ASSERT_EQ(addr.get_port(), 8080);

    auto sa = addr.to_ipv4_address();
    ASSERT_EQ(sa.sin_family, AF_INET);
    ASSERT_EQ(ntohs(sa.sin_port), 8080);

    auto storage = addr.to_sockaddr();
    auto *sa4 = reinterpret_cast<const sockaddr_in *>(&storage);
    ASSERT_EQ(sa4->sin_family, AF_INET);

    PASS();
}

static void test_inet_address_ipv6_explicit()
{
    TEST("InetAddress IPv6 explicit");
    InetAddress addr("::1", 9090);
    ASSERT_EQ(addr.family(), AddressFamily::ipv6);
    ASSERT_TRUE(addr.is_ipv6());
    ASSERT_EQ(addr.get_ip(), "::1");
    ASSERT_EQ(addr.get_port(), 9090);

    auto sa6 = addr.to_ipv6_address();
    ASSERT_EQ(sa6.sin6_family, AF_INET6);
    ASSERT_EQ(ntohs(sa6.sin6_port), 9090);

    auto storage = addr.to_sockaddr();
    auto *sa = reinterpret_cast<const sockaddr_in6 *>(&storage);
    ASSERT_EQ(sa->sin6_family, AF_INET6);

    PASS();
}

static void test_inet_address_ipv6_full()
{
    TEST("InetAddress IPv6 full address");
    InetAddress addr("fe80::1", 1234);
    ASSERT_EQ(addr.family(), AddressFamily::ipv6);
    ASSERT_TRUE(addr.is_ipv6());
    ASSERT_EQ(addr.get_ip(), "fe80::1");
    ASSERT_EQ(addr.get_port(), 1234);
    PASS();
}

static void test_inet_address_from_sockaddr_in()
{
    TEST("InetAddress from sockaddr_in");
    sockaddr_in sa4{};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(443);
    inet_pton(AF_INET, "192.168.1.1", &sa4.sin_addr);

    InetAddress addr(sa4);
    ASSERT_EQ(addr.family(), AddressFamily::ipv4);
    ASSERT_EQ(addr.get_port(), 443);
    ASSERT_EQ(addr.get_ip(), "192.168.1.1");
    PASS();
}

static void test_inet_address_from_sockaddr_in6()
{
    TEST("InetAddress from sockaddr_in6");
    sockaddr_in6 sa6{};
    sa6.sin6_family = AF_INET6;
    sa6.sin6_port = htons(8443);
    inet_pton(AF_INET6, "2001:db8::1", &sa6.sin6_addr);

    InetAddress addr(sa6);
    ASSERT_EQ(addr.family(), AddressFamily::ipv6);
    ASSERT_TRUE(addr.is_ipv6());
    ASSERT_EQ(addr.get_port(), 8443);
    ASSERT_EQ(addr.get_ip(), "2001:db8::1");
    PASS();
}

static void test_inet_address_from_sockaddr_storage()
{
    TEST("InetAddress from sockaddr_storage (IPv4)");
    sockaddr_storage storage{};
    auto *sa4 = reinterpret_cast<sockaddr_in *>(&storage);
    sa4->sin_family = AF_INET;
    sa4->sin_port = htons(80);
    inet_pton(AF_INET, "10.0.0.1", &sa4->sin_addr);

    InetAddress addr(storage);
    ASSERT_EQ(addr.family(), AddressFamily::ipv4);
    ASSERT_EQ(addr.get_port(), 80);
    ASSERT_EQ(addr.get_ip(), "10.0.0.1");
    PASS();
}

static void test_inet_address_from_sockaddr_storage_ipv6()
{
    TEST("InetAddress from sockaddr_storage (IPv6)");
    sockaddr_storage storage{};
    auto *sa6 = reinterpret_cast<sockaddr_in6 *>(&storage);
    sa6->sin6_family = AF_INET6;
    sa6->sin6_port = htons(8080);
    inet_pton(AF_INET6, "::ffff:192.0.2.1", &sa6->sin6_addr);

    InetAddress addr(storage);
    ASSERT_EQ(addr.family(), AddressFamily::ipv6);
    ASSERT_TRUE(addr.is_ipv6());
    ASSERT_EQ(addr.get_port(), 8080);
    PASS();
}

static void test_inet_address_copy_move()
{
    TEST("InetAddress copy/move (IPv6)");
    InetAddress orig("::1", 1234);
    InetAddress copy(orig);
    ASSERT_EQ(copy.family(), AddressFamily::ipv6);
    ASSERT_EQ(copy.get_ip(), "::1");
    ASSERT_EQ(copy.get_port(), 1234);

    InetAddress moved(std::move(copy));
    ASSERT_EQ(moved.family(), AddressFamily::ipv6);
    ASSERT_EQ(moved.get_ip(), "::1");
    ASSERT_EQ(moved.get_port(), 1234);

    InetAddress assigned;
    assigned = orig;
    ASSERT_EQ(assigned.family(), AddressFamily::ipv6);
    ASSERT_EQ(assigned.get_ip(), "::1");

    InetAddress move_assigned;
    move_assigned = std::move(assigned);
    ASSERT_EQ(move_assigned.family(), AddressFamily::ipv6);
    ASSERT_EQ(move_assigned.get_ip(), "::1");
    PASS();
}

static void test_inet_address_comparison()
{
    TEST("InetAddress comparison");
    InetAddress a4("127.0.0.1", 80);
    InetAddress b4("127.0.0.1", 80);
    InetAddress c4("127.0.0.2", 80);
    InetAddress d4("127.0.0.1", 81);

    ASSERT_TRUE(a4 == b4);
    ASSERT_TRUE(a4 != c4);
    ASSERT_TRUE(a4 != d4);

    InetAddress a6("::1", 80);
    InetAddress b6("::1", 80);
    ASSERT_TRUE(a6 == b6);
    ASSERT_TRUE(a4 != a6);

    PASS();
}

static void test_inet_address_to_address_key()
{
    TEST("InetAddress to_address_key");
    InetAddress a4("127.0.0.1", 80);
    ASSERT_EQ(a4.to_address_key(), "127.0.0.1:80");

    InetAddress a6("::1", 80);
    std::string key6 = a6.to_address_key();
    ASSERT_TRUE(key6 == "[::1]:80");
    PASS();
}

static void test_inet_address_get_net_ip_ipv4()
{
    TEST("InetAddress get_net_ip (IPv4)");
    InetAddress addr("127.0.0.1", 80);
    uint32_t net_ip = addr.get_net_ip();
    in_addr expected;
    inet_pton(AF_INET, "127.0.0.1", &expected);
    ASSERT_EQ(net_ip, expected.s_addr);
    PASS();
}

static void test_inet_address_set_addr()
{
    TEST("InetAddress set_addr (IPv4 -> IPv6)");
    InetAddress addr("127.0.0.1", 80);
    ASSERT_EQ(addr.family(), AddressFamily::ipv4);

    addr.set_addr("::1", 9090);
    ASSERT_EQ(addr.family(), AddressFamily::ipv6);
    ASSERT_EQ(addr.get_ip(), "::1");
    ASSERT_EQ(addr.get_port(), 9090);
    PASS();
}

static void test_inet_address_normalize_host_ipv4()
{
    TEST("InetAddress normalize_host (IPv4 literal)");
    std::string result = InetAddress::normalize_host("127.0.0.1");
    ASSERT_EQ(result, "127.0.0.1");
    PASS();
}

static void test_inet_address_normalize_host_ipv6()
{
    TEST("InetAddress normalize_host (IPv6 literal)");
    std::string result = InetAddress::normalize_host("::1");
    ASSERT_EQ(result, "::1");
    PASS();
}

static void test_socket_ops_ipv4()
{
    TEST("socket_ops IPv4 TCP/UDP create");
    int tcp_fd = socket::create_ipv4_tcp_socket(true);
    ASSERT_TRUE(tcp_fd >= 0);
    socket::close_fd(tcp_fd);

    int udp_fd = socket::create_ipv4_udp_socket(true);
    ASSERT_TRUE(udp_fd >= 0);
    socket::close_fd(udp_fd);
    PASS();
}

static void test_socket_ops_ipv6()
{
    TEST("socket_ops IPv6 TCP/UDP create");
    int tcp_fd = socket::create_ipv6_tcp_socket(true);
    ASSERT_TRUE(tcp_fd >= 0);
    socket::close_fd(tcp_fd);

    int udp_fd = socket::create_ipv6_udp_socket(true);
    ASSERT_TRUE(udp_fd >= 0);
    socket::close_fd(udp_fd);
    PASS();
}

static void test_socket_ops_ipv6_bind_loopback()
{
    TEST("socket_ops IPv6 bind [::1]");
    int fd = socket::create_ipv6_tcp_socket(true);
    ASSERT_TRUE(fd >= 0);
    socket::set_reuse(fd, true);

    InetAddress addr("::1", 0);
    int ret = socket::bind(fd, addr);
    ASSERT_EQ(ret, 0);

    socket::close_fd(fd);
    PASS();
}

static void test_socket_listen_options()
{
    TEST("socket ListenOptions apply");
    int fd = socket::create_ipv4_tcp_socket(false);
    ASSERT_TRUE(fd >= 0);

    ListenOptions options;
    options.reuse_addr = true;
    options.reuse_port = false;
    options.non_block = true;
    ASSERT_TRUE(socket::apply_listen_options(fd, options));

    InetAddress addr("127.0.0.1", 0);
    ASSERT_EQ(socket::bind(fd, addr), 0);
    ASSERT_EQ(socket::listen(fd, options.backlog), 0);

    socket::close_fd(fd);
    PASS();
}

static void test_socket_reuse_port_explicit()
{
    TEST("socket reuse_port is explicit");
    int fd = socket::create_ipv4_tcp_socket(false);
    ASSERT_TRUE(fd >= 0);

    ASSERT_TRUE(socket::set_reuse_addr(fd, true));
#ifdef _WIN32
    ASSERT_TRUE(!socket::set_reuse_port(fd, true));
#else
    (void)socket::set_reuse_port(fd, false);
#endif

    socket::close_fd(fd);
    PASS();
}

static void test_socket_ipv4_bind()
{
    TEST("Socket IPv4 bind");
    Socket sock("127.0.0.1", 0, false);
    ASSERT_TRUE(sock.valid());
    ASSERT_TRUE(sock.bind());
    PASS();
}

static void test_socket_ipv6_bind()
{
    TEST("Socket IPv6 bind");
    Socket sock("::1", 0, false);
    ASSERT_TRUE(sock.valid());
    ASSERT_TRUE(sock.bind());
    PASS();
}

static void test_socket_ipv4_bind_listen()
{
    TEST("Socket IPv4 bind+listen");
    Socket sock("127.0.0.1", 0, false);
    ASSERT_TRUE(sock.valid());
    ASSERT_TRUE(sock.bind());
    ASSERT_TRUE(sock.listen());
    PASS();
}

static void test_socket_ipv6_bind_listen()
{
    TEST("Socket IPv6 bind+listen");
    Socket sock("::1", 0, false);
    ASSERT_TRUE(sock.valid());
    ASSERT_TRUE(sock.bind());
    ASSERT_TRUE(sock.listen());
    PASS();
}

static void test_socket_ipv4_udp()
{
    TEST("Socket IPv4 UDP bind");
    Socket sock("127.0.0.1", 0, true);
    ASSERT_TRUE(sock.valid());
    ASSERT_TRUE(sock.bind());
    PASS();
}

static void test_socket_ipv6_udp()
{
    TEST("Socket IPv6 UDP bind");
    Socket sock("::1", 0, true);
    ASSERT_TRUE(sock.valid());
    ASSERT_TRUE(sock.bind());
    PASS();
}

static void test_socket_accept_returns_sockaddr_storage()
{
    TEST("Socket accept uses sockaddr_storage");
    Socket listen_sock("127.0.0.1", 0, false);
    ASSERT_TRUE(listen_sock.valid());
    ASSERT_TRUE(listen_sock.bind());
    ASSERT_TRUE(listen_sock.listen());

    sockaddr_storage peer{};
    int conn_fd = listen_sock.accept(peer);
    ASSERT_TRUE(conn_fd < 0);
    PASS();
}

int main()
{
    std::cout << "=== IPv4/IPv6 Dual-Stack Test Matrix ===" << std::endl;

    std::cout << "\n-- InetAddress tests --" << std::endl;
    test_inet_address_ipv4_default();
    test_inet_address_ipv4_explicit();
    test_inet_address_ipv6_explicit();
    test_inet_address_ipv6_full();
    test_inet_address_from_sockaddr_in();
    test_inet_address_from_sockaddr_in6();
    test_inet_address_from_sockaddr_storage();
    test_inet_address_from_sockaddr_storage_ipv6();
    test_inet_address_copy_move();
    test_inet_address_comparison();
    test_inet_address_to_address_key();
    test_inet_address_get_net_ip_ipv4();
    test_inet_address_set_addr();
    test_inet_address_normalize_host_ipv4();
    test_inet_address_normalize_host_ipv6();

    std::cout << "\n-- socket_ops tests --" << std::endl;
    test_socket_ops_ipv4();
    test_socket_ops_ipv6();
    test_socket_ops_ipv6_bind_loopback();
    test_socket_listen_options();
    test_socket_reuse_port_explicit();

    std::cout << "\n-- Socket tests --" << std::endl;
    test_socket_ipv4_bind();
    test_socket_ipv6_bind();
    test_socket_ipv4_bind_listen();
    test_socket_ipv6_bind_listen();
    test_socket_ipv4_udp();
    test_socket_ipv6_udp();
    test_socket_accept_returns_sockaddr_storage();

    std::cout << "\n=== Results: " << tests_passed << " passed, " << tests_failed << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
