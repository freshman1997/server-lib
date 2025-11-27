#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifndef _WIN32
#include <signal.h>
#else
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

#include "http_server.h"

using namespace yuan;

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif

    net::http::HttpServer server;
    if (!server.init(45005)) {
        std::cout << " init failed " << std::endl;
        return 1;
    }

    server.serve();
    
#ifdef _WIN32
    WSACleanup();
#endif
}