#include "client/ftp_client.h"
#include "server/context.h"
#include "server/ftp_server.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

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
    const auto root = fs::temp_directory_path() / "ftp_auto_fallback_root";
    const auto download_dir = fs::temp_directory_path() / "ftp_auto_fallback_downloads";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(download_dir, ec);
    fs::create_directories(root, ec);
    fs::create_directories(download_dir, ec);

    std::ofstream(root / "hello.txt") << "fallback-test-data";
    std::ofstream(download_dir / "upload_src.txt") << "fallback-upload-data";

    auto ctx = ServerContext::get_instance();
    ctx->set_server_work_dir(root.generic_string());
    ctx->set_auth_credential("tester", "secret");
    ctx->set_stream_port_range(20001, 20001);
    ctx->get_next_stream_port();
    ctx->get_next_stream_port();

    FtpServer server;
    std::thread server_thread([&]() {
        server.serve(12126);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    FtpClient client;
    require(client.connect("127.0.0.1", 12126), "connect failed");
    require(client.login("tester", "secret"), "login failed");

    const auto downloaded = download_dir / "downloaded.txt";
    require(client.download("hello.txt", downloaded.generic_string()), "auto-select download failed");
    require(wait_until([&]() {
        return fs::exists(downloaded) && read_file(downloaded) == "fallback-test-data";
    }, 5000), "download content mismatch");

    const auto uploaded = root / "uploaded.txt";
    require(client.upload((download_dir / "upload_src.txt").generic_string(), "uploaded.txt"), "auto-select upload failed");
    require(wait_until([&]() {
        return fs::exists(uploaded) && read_file(uploaded) == "fallback-upload-data";
    }, 15000), "upload content mismatch");

    client.quit();
    server.quit();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    ctx->clear_auth_credential();
    fs::remove_all(root, ec);
    fs::remove_all(download_dir, ec);
    return 0;
}