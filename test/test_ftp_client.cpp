#include "net/ftp/client/command_scanner.h"
#include "net/ftp/client/ftp_client.h"
#include <iostream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <signal.h>
#else
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
    net::ftp::FtpClient client;
    std::thread runner([&client]() {
        client.connect("192.168.96.1", 12123);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    net::ftp::CommandScanner scanner;
    while (true) {
        if (client.is_ok()) {
            client.send_command(scanner.simpleCommand());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            std::cout << "client didn't connected yet\n";
            break;
        }
    }
    runner.join();
    
#ifdef _WIN32
    WSACleanup();
#endif
}