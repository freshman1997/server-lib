#include "client/command_scanner.h"
#include "client/ftp_client.h"
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif
    yuan::net::ftp::FtpClient client;
    std::thread runner([&client]() {
        client.connect("192.168.96.1", 12123);
    });

    if (!client.wait_until_connected(2000)) {
        std::cout << "client didn't connected yet\n";
        runner.join();
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

    yuan::net::ftp::CommandScanner scanner;
    while (true) {
        if (client.is_ok()) {
            client.send_command(scanner.simpleCommand());
        } else {
            break;
        }
    }
    runner.join();
    
#ifdef _WIN32
    WSACleanup();
#endif
}
