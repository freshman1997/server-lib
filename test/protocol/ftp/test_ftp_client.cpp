#include "client/ftp_client.h"
#include <iostream>
#include "common/winsock_guard.h"

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

int main()
{
    const test::common::WinsockGuard winsock;
    if (!winsock.ok()) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    yuan::net::ftp::FtpClient client;
    if (!client.connect("192.168.96.1", 12123)) {
        std::cout << "ftp client failed to connect\n";
        return 1;
    }

    std::cout << "ftp client connected\n";
    client.quit();
    return 0;
}
