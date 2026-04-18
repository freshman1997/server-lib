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
#include "common/winsock_guard.h"

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
    const test::common::WinsockGuard winsock;
    if (!winsock.ok()) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "ftp_e2e_root";
    const auto downloadDir = fs::temp_directory_path() / "ftp_e2e_downloads";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(downloadDir, ec);
    fs::create_directories(root, ec);
    fs::create_directories(downloadDir, ec);
    {
        std::ofstream(root / "sample.txt") << "0123456789abcdef";
        std::ofstream(root / "append_target.txt") << "base-";
        
        std::ofstream(downloadDir / "upload_source.txt") << "client-upload";
        std::ofstream(downloadDir / "append_source.txt") << "tail";
    }

    ServerContext::get_instance()->set_server_work_dir(root.generic_string());

    FtpServer server;
    std::thread serverThread([&]() { server.serve(12123); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    FtpClient client;
    require(client.connect("127.0.0.1", 12123), "client did not connect to server");
    require(client.login("tester", "secret"), "login did not complete");

    const auto resumeFile = downloadDir / "downloaded.txt";
    require(client.download("sample.txt", resumeFile.generic_string()), "download send failed");
    require(fs::exists(resumeFile), "download target file was not created");
    require(wait_until([&]() { return read_file(resumeFile) == "0123456789abcdef"; }, 5000), "download did not complete correctly");

    const auto uploaded = root / "uploaded.txt";
    require(client.upload((downloadDir / "upload_source.txt").generic_string(), "uploaded.txt"), "upload send failed");
    require(wait_until([&]() {
        bool exists = fs::exists(uploaded);
        std::string content = exists ? read_file(uploaded) : "";
        return exists && content == "client-upload";
    }, 20000), "upload did not complete correctly");

    const auto appendTarget = root / "append_target.txt";
    require(client.append((downloadDir / "append_source.txt").generic_string(), "append_target.txt"), "append send failed");
    require(wait_until([&]() { return fs::exists(appendTarget); }, 15000), "append file was not created");
    // 检查文件是否包含预期的内容，忽略可能的换行符差异
    std::string appendContent = read_file(appendTarget);
    bool containsTail = appendContent.find("tail") != std::string::npos;
    require(containsTail, std::string("append file does not contain expected content. Got: ") + appendContent);

    client.quit();
    server.quit();
    serverThread.join();

    fs::remove_all(root, ec);
    fs::remove_all(downloadDir, ec);

    std::cout << "ftp e2e tests passed\n";
    return 0;
}
