#include "client/ftp_client.h"
#include <iostream>


#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

int main()
{
    
    yuan::net::ftp::FtpClient client;
    if (!client.connect("192.168.96.1", 12123)) {
        std::cout << "ftp client failed to connect\n";
        return 1;
    }

    std::cout << "ftp client connected\n";
    client.quit();
    return 0;
}
