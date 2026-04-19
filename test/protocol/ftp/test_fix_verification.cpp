#include "client/ftp_client.h"
#include "server/ftp_server.h"
#include "server/context.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>


#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

using namespace yuan::net::ftp;

namespace
{
    bool wait_until(const std::function<bool()> &pred, int timeout_ms = 10000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return pred();
    }

    void require(bool cond, const std::string &message)
    {
        if (!cond) {
            throw std::runtime_error(message);
        }
    }

    std::string read_file(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
}

int main()
{
    

    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "ftp_test_fix";
    const auto downloadDir = fs::temp_directory_path() / "ftp_test_fix_downloads";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(downloadDir, ec);
    fs::create_directories(root, ec);
    fs::create_directories(downloadDir, ec);
    {
        std::ofstream(root / "sample.txt") << "Hello FTP Test!";
    }

    ServerContext::get_instance()->set_server_work_dir(root.generic_string());

    FtpServer server;
    std::thread serverThread([&]() { server.serve(12124); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    FtpClient client;
    require(client.connect("127.0.0.1", 12124), "client did not connect to server");
    require(client.login("tester", "secret"), "login did not complete");

    const auto resumeFile = downloadDir / "downloaded.txt";
    require(client.download("sample.txt", resumeFile.generic_string()), "download send failed");
    require(fs::exists(resumeFile), "downloaded file was not created");
    require(wait_until([&]() { return fs::exists(resumeFile) && read_file(resumeFile) == "Hello FTP Test!"; }, 15000), "download did not complete correctly");

    std::cout << "✓ FTP download test passed (no crash!)\n";

    client.quit();
    server.quit();
    serverThread.join();

    fs::remove_all(root, ec);
    fs::remove_all(downloadDir, ec);

    std::cout << "✓ All tests passed successfully\n";
    return 0;
}
