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
    const auto root = fs::temp_directory_path() / "ftp_ipv6_root";
    const auto download_dir = fs::temp_directory_path() / "ftp_ipv6_downloads";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(download_dir, ec);
    fs::create_directories(root, ec);
    fs::create_directories(download_dir, ec);

    std::ofstream(root / "ipv6_test.txt") << "ipv6-passive-data";

    auto ctx = ServerContext::get_instance();
    ctx->set_server_work_dir(root.generic_string());
    ctx->set_auth_credential("tester", "secret");

    FtpServer server;
    std::thread server_thread([&]() {
        server.serve("::1", 12127);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    require(server.is_ok(), "ipv6 server did not start");

    FtpClient client;
    require(client.connect("::1", 12127), "ipv6 connect failed");
    require(client.login("tester", "secret"), "ipv6 login failed");

    std::string listing = client.list();
    require(!listing.empty(), "ipv6 list should not be empty");

    const auto downloaded = download_dir / "ipv6_downloaded.txt";
    require(client.download("ipv6_test.txt", downloaded.generic_string()), "ipv6 download failed");
    require(wait_until([&]() {
        return fs::exists(downloaded) && read_file(downloaded) == "ipv6-passive-data";
    }, 5000), "ipv6 download content mismatch");

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