#include "server/ftp_server.h"

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

    yuan::net::ftp::FtpServer server;
    server.serve(12123);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}