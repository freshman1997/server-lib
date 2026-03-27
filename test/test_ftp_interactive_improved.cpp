#include "client/ftp_client.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

using namespace yuan::net::ftp;

class InteractiveFtpClient
{
private:
    FtpClient client_;
    bool connected_ = false;
    bool logged_in_ = false;
    std::thread connect_thread_;

public:
    ~InteractiveFtpClient()
    {
        if (connect_thread_.joinable()) {
            connect_thread_.join();
        }
    }

    bool connect(const std::string& host, int port = 21)
    {
        if (connect_thread_.joinable()) {
            connect_thread_.join();
        }

        std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;
        
        connect_thread_ = std::thread([this, host, port]() {
            client_.connect(host, port);
        });

        // Wait for connection
        for (int i = 0; i < 50; ++i) {
            if (client_.is_ok()) {
                connected_ = true;
                std::cout << "Connected!" << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "Connection failed or timed out" << std::endl;
        connected_ = false;
        return false;
    }

    bool login(const std::string& username, const std::string& password)
    {
        if (!connected_) {
            std::cout << "Not connected. Use 'connect' first." << std::endl;
            return false;
        }

        if (!client_.login(username, password)) {
            std::cout << "Failed to send login command" << std::endl;
            return false;
        }

        // Wait for login response
        for (int i = 0; i < 50; ++i) {
            const auto* ctx = client_.get_client_context();
            if (ctx && !ctx->responses_.empty()) {
                const auto& last = ctx->responses_.back();
                if (last.code_ == 230) {
                    logged_in_ = true;
                    std::cout << "Login successful!" << std::endl;
                    return true;
                } else if (last.code_ == 530) {
                    std::cout << "Login failed: " << last.body_ << std::endl;
                    return false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Login timed out" << std::endl;
        return false;
    }

    bool list(const std::string& path = "")
    {
        if (!logged_in_) {
            std::cout << "Not logged in. Use 'login' first." << std::endl;
            return false;
        }

        std::cout << "Listing directory..." << std::endl;
        
        if (!client_.list(path)) {
            std::cout << "Failed to send list command" << std::endl;
            return false;
        }

        // Wait for list completion
        for (int i = 0; i < 50; ++i) {
            const auto* ctx = client_.get_client_context();
            if (ctx && !ctx->list_output_.empty()) {
                std::cout << ctx->list_output_ << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "List timed out or empty" << std::endl;
        return false;
    }

    bool download(const std::string& remote_path, const std::string& local_path)
    {
        if (!logged_in_) {
            std::cout << "Not logged in. Use 'login' first." << std::endl;
            return false;
        }

        std::cout << "Downloading " << remote_path << " to " << local_path << "..." << std::endl;
        
        if (!client_.download(remote_path, local_path)) {
            std::cout << "Failed to send download command" << std::endl;
            return false;
        }

        // Wait for download completion
        for (int i = 0; i < 100; ++i) {
            std::ifstream file(local_path, std::ios::binary);
            if (file.good()) {
                auto size = std::filesystem::file_size(local_path);
                std::cout << "Download complete! Size: " << size << " bytes" << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Download timed out" << std::endl;
        return false;
    }

    bool upload(const std::string& local_path, const std::string& remote_path)
    {
        if (!logged_in_) {
            std::cout << "Not logged in. Use 'login' first." << std::endl;
            return false;
        }

        std::ifstream file(local_path, std::ios::binary);
        if (!file.good()) {
            std::cout << "Local file not found: " << local_path << std::endl;
            return false;
        }

        std::cout << "Uploading " << local_path << " to " << remote_path << "..." << std::endl;
        
        if (!client_.upload(local_path, remote_path)) {
            std::cout << "Failed to send upload command" << std::endl;
            return false;
        }

        // Wait for upload completion
        for (int i = 0; i < 100; ++i) {
            if (!client_.is_transfer_in_progress()) {
                std::cout << "Upload complete!" << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Upload timed out" << std::endl;
        return false;
    }

    bool append(const std::string& local_path, const std::string& remote_path)
    {
        if (!logged_in_) {
            std::cout << "Not logged in. Use 'login' first." << std::endl;
            return false;
        }

        std::ifstream file(local_path, std::ios::binary);
        if (!file.good()) {
            std::cout << "Local file not found: " << local_path << std::endl;
            return false;
        }

        std::cout << "Appending " << local_path << " to " << remote_path << "..." << std::endl;
        
        if (!client_.append(local_path, remote_path)) {
            std::cout << "Failed to send append command" << std::endl;
            return false;
        }

        // Wait for append completion
        for (int i = 0; i < 100; ++i) {
            if (!client_.is_transfer_in_progress()) {
                std::cout << "Append complete!" << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Append timed out" << std::endl;
        return false;
    }

    bool nlist(const std::string& path = "")
    {
        if (!logged_in_) {
            std::cout << "Not logged in. Use 'login' first." << std::endl;
            return false;
        }

        std::cout << "Name listing..." << std::endl;
        
        if (!client_.nlist(path)) {
            std::cout << "Failed to send nlist command" << std::endl;
            return false;
        }

        // Wait for nlist completion
        for (int i = 0; i < 50; ++i) {
            const auto* ctx = client_.get_client_context();
            if (ctx && !ctx->list_output_.empty()) {
                std::cout << ctx->list_output_ << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Nlist timed out or empty" << std::endl;
        return false;
    }

    void disconnect()
    {
        if (connected_) {
            client_.quit();
            connected_ = false;
            logged_in_ = false;
            std::cout << "Disconnected from server" << std::endl;
        }
    }

    void quit()
    {
        if (connected_) {
            std::cout << "Sending QUIT command..." << std::endl;
            client_.send_command("QUIT\r\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            client_.quit();
            connected_ = false;
            logged_in_ = false;
        }
    }

    bool is_connected() const { return connected_; }
    bool is_logged_in() const { return logged_in_; }
};

std::vector<std::string> split_command(const std::string& input)
{
    std::vector<std::string> result;
    std::istringstream iss(input);
    std::string token;
    while (iss >> token) {
        result.push_back(token);
    }
    return result;
}

void print_help()
{
    std::cout << "\n============================================" << std::endl;
    std::cout << "   Available Commands:" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "  connect <host> [port]   - Connect to FTP server" << std::endl;
    std::cout << "                          (default port: 21)" << std::endl;
    std::cout << "  login <user> <pass>    - Login to server" << std::endl;
    std::cout << "  list [path]            - List directory contents" << std::endl;
    std::cout << "  nlist [path]           - List file names only" << std::endl;
    std::cout << "  download <r> <l>       - Download file (r=remote, l=local)" << std::endl;
    std::cout << "  upload <l> <r>         - Upload file (l=local, r=remote)" << std::endl;
    std::cout << "  append <l> <r>         - Append to file on server" << std::endl;
    std::cout << "  disconnect              - Disconnect from server (can reconnect)" << std::endl;
    std::cout << "  help                   - Show this help message" << std::endl;
    std::cout << "  quit                    - Send QUIT command and exit" << std::endl;
    std::cout << "  exit                    - Exit program completely" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  connect 127.0.0.1 12123" << std::endl;
    std::cout << "  login tester secret" << std::endl;
    std::cout << "  list" << std::endl;
    std::cout << "  download sample.txt ./downloaded.txt" << std::endl;
    std::cout << "  upload ./file.txt uploaded.txt" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << std::endl;
}

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa); iResult != NO_ERROR) {
        std::cerr << "WSAStartup failed: " << iResult << std::endl;
        return 1;
    }
#endif

    InteractiveFtpClient client;
    
    std::cout << "============================================" << std::endl;
    std::cout << "   Interactive FTP Client" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "Type 'help' for available commands" << std::endl;
    std::cout << std::endl;

    std::string input;
    while (true) {
        try {
            std::string prompt;
            if (client.is_logged_in()) {
                prompt = "ftp> ";
            } else if (client.is_connected()) {
                prompt = "ftp-connected> ";
            } else {
                prompt = "ftp-disconnected> ";
            }
            
            std::cout << prompt;
            std::getline(std::cin, input);

            if (input.empty()) {
                continue;
            }

            auto args = split_command(input);
            if (args.empty()) {
                continue;
            }

            const std::string& cmd = args[0];

            if (cmd == "exit") {
                client.disconnect();
                std::cout << "Goodbye!" << std::endl;
                break;
            } else if (cmd == "quit") {
                client.quit();
                std::cout << "Goodbye!" << std::endl;
                break;
            } else if (cmd == "help" || cmd == "h" || cmd == "?") {
                print_help();
            } else if (cmd == "connect") {
                if (args.size() < 2) {
                    std::cout << "Usage: connect <host> [port]" << std::endl;
                    continue;
                }
                int port = 21;
                if (args.size() >= 3) {
                    try {
                        port = std::stoi(args[2]);
                    } catch (...) {
                        std::cout << "Invalid port number: " << args[2] << std::endl;
                        continue;
                    }
                }
                client.connect(args[1], port);
            } else if (cmd == "disconnect") {
                client.disconnect();
            } else if (cmd == "login") {
                if (args.size() < 3) {
                    std::cout << "Usage: login <username> <password>" << std::endl;
                    continue;
                }
                client.login(args[1], args[2]);
            } else if (cmd == "list" || cmd == "ls") {
                std::string path = (args.size() >= 2) ? args[1] : "";
                client.list(path);
            } else if (cmd == "nlist" || cmd == "nlst") {
                std::string path = (args.size() >= 2) ? args[1] : "";
                client.nlist(path);
            } else if (cmd == "download" || cmd == "get") {
                if (args.size() < 3) {
                    std::cout << "Usage: download <remote_file> <local_path>" << std::endl;
                    continue;
                }
                client.download(args[1], args[2]);
            } else if (cmd == "upload" || cmd == "put") {
                if (args.size() < 3) {
                    std::cout << "Usage: upload <local_path> <remote_file>" << std::endl;
                    continue;
                }
                client.upload(args[1], args[2]);
            } else if (cmd == "append") {
                if (args.size() < 3) {
                    std::cout << "Usage: append <local_path> <remote_file>" << std::endl;
                    continue;
                }
                client.append(args[1], args[2]);
            } else {
                std::cout << "Unknown command: " << cmd << std::endl;
                std::cout << "Type 'help' for available commands" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << std::endl;
            std::cout << "Please try again" << std::endl;
        } catch (...) {
            std::cout << "Unknown error occurred" << std::endl;
            std::cout << "Please try again" << std::endl;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
