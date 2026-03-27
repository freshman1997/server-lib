#include "server/ftp_server.h"
#include "server/context.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "ftp_e2e_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    ServerContext::get_instance()->set_server_work_dir(root.generic_string());

    // Create initial test files for download testing
    std::ofstream(root / "sample.txt") << "0123456789abcdef";
    std::ofstream(root / "append_target.txt") << "base-";

    FtpServer server;
    std::cout << "FTP Server starting on port 12123...\n";
    std::cout << "Root directory: " << root.generic_string() << "\n";

    server.serve(12123);

    fs::remove_all(root, ec);

#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "FTP Server stopped\n";
    return 0;
}
