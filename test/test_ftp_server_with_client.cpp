#include "server/ftp_server.h"
#include "server/context.h"

#include <filesystem>
#include <iostream>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

using namespace yuan::net::ftp;

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa); iResult != NO_ERROR) {
        std::cerr << "WSAStartup failed: " << iResult << '\n';
        return 1;
    }
#endif

    // 设置服务器根目录
    std::string root_dir = "E:/k5client";
    std::error_code ec;
    std::filesystem::create_directories(root_dir, ec);
    
    ServerContext::get_instance()->set_server_work_dir(root_dir);
    
    // 创建一些测试文件
    std::ofstream(root_dir + "/readme.txt") << "Welcome to FTP Server!\nThis is a test file.";
    std::ofstream(root_dir + "/data.txt") << "Sample data content";

    std::cout << "============================================" << std::endl;
    std::cout << "   FTP Server with Interactive Client" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "Server root: " << root_dir << std::endl;
    std::cout << "Port: 12123" << std::endl;
    std::cout << "\nWindows FTP Client commands:" << std::endl;
    std::cout << "  ftp 127.0.0.1 12123" << std::endl;
    std::cout << "  open 127.0.0.1 12123" << std::endl;
    std::cout << "\n============================================" << std::endl;

    FtpServer server;
    server.serve(12123);

#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "Server stopped" << std::endl;
    return 0;
}
