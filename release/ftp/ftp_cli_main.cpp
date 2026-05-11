#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "client/ftp_client.h"

namespace
{
    struct CliConfig
    {
        std::string host = "127.0.0.1";
        int port = 2121;
        std::string username = "tester";
        std::string password = "secret";
        std::string list_path;
        std::string nlist_path;
        std::string download_remote;
        std::string download_local;
        std::string upload_local;
        std::string upload_remote;
        std::string append_local;
        std::string append_remote;
        yuan::net::ftp::FtpClient::DataMode data_mode = yuan::net::ftp::FtpClient::DataMode::auto_select;
        bool interactive = false;
    };

    std::string read_env_string(const char *name, const std::string &default_value = {})
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

    int read_env_int(const char *name, int default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0') {
            return default_value;
        }
        try {
            return std::stoi(raw);
        } catch (...) {
            return default_value;
        }
    }

    bool parse_int_value(const std::string &raw, int &out)
    {
        try {
            size_t pos = 0;
            const int value = std::stoi(raw, &pos);
            if (pos != raw.size()) {
                return false;
            }
            out = value;
            return true;
        } catch (...) {
            return false;
        }
    }

    std::vector<std::string> split_command(const std::string &line)
    {
        std::vector<std::string> result;
        std::istringstream iss(line);
        std::string token;
        while (iss >> token) {
            result.push_back(token);
        }
        return result;
    }

    void print_usage(const char *program)
    {
        std::cout
            << "release_ftp_cli\n"
            << "usage:\n"
            << "  " << program << " [options]\n\n"
            << "options:\n"
            << "      --host <ip>                  FTP server host\n"
            << "  -p, --port <port>                FTP server port\n"
            << "  -u, --user <name>                Username\n"
            << "      --password <password>        Password\n"
            << "      --list [path]                Run LIST and print result\n"
            << "      --nlist [path]               Run NLST and print result\n"
            << "      --download <remote> <local>  Download file\n"
            << "      --upload <local> <remote>    Upload file\n"
            << "      --append <local> <remote>    Append local file to remote file\n"
            << "      --force-active               Force active data mode (EPRT/PORT)\n"
            << "      --force-passive              Force passive data mode (EPSV/PASV)\n"
            << "  -i, --interactive                Interactive shell mode\n"
            << "      --version                    Print version\n"
            << "  -h, --help                       Show this help\n\n"
            << "env defaults:\n"
            << "  YUAN_FTP_HOST, YUAN_FTP_PORT, YUAN_FTP_USER, YUAN_FTP_PASSWORD\n";
    }

    void print_repl_help()
    {
        std::cout
            << "commands:\n"
            << "  list [path]\n"
            << "  nlist [path]\n"
            << "  get <remote> <local>\n"
            << "  put <local> <remote>\n"
            << "  append <local> <remote>\n"
            << "  quit\n";
    }

    bool run_interactive(yuan::net::ftp::FtpClient &client)
    {
        std::cout << "connected. type 'help' for commands\n";
        std::string line;
        while (true) {
            std::cout << "ftp> ";
            if (!std::getline(std::cin, line)) {
                break;
            }
            const auto args = split_command(line);
            if (args.empty()) {
                continue;
            }
            if (args[0] == "quit" || args[0] == "exit") {
                break;
            }
            if (args[0] == "help") {
                print_repl_help();
                continue;
            }
            if (args[0] == "list") {
                const std::string path = args.size() > 1 ? args[1] : "";
                std::cout << client.list(path);
                continue;
            }
            if (args[0] == "nlist") {
                const std::string path = args.size() > 1 ? args[1] : "";
                std::cout << client.nlist(path);
                continue;
            }
            if (args[0] == "get" && args.size() >= 3) {
                std::cout << (client.download(args[1], args[2]) ? "ok\n" : "failed\n");
                continue;
            }
            if (args[0] == "put" && args.size() >= 3) {
                std::cout << (client.upload(args[1], args[2]) ? "ok\n" : "failed\n");
                continue;
            }
            if (args[0] == "append" && args.size() >= 3) {
                std::cout << (client.append(args[1], args[2]) ? "ok\n" : "failed\n");
                continue;
            }
            std::cout << "unknown command\n";
        }
        return true;
    }
}

int main(int argc, char **argv)
{
    CliConfig cfg;
    cfg.host = read_env_string("YUAN_FTP_HOST", cfg.host);
    cfg.port = read_env_int("YUAN_FTP_PORT", cfg.port);
    cfg.username = read_env_string("YUAN_FTP_USER", cfg.username);
    cfg.password = read_env_string("YUAN_FTP_PASSWORD", cfg.password);

    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        auto need_value = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << '\n';
                return {};
            }
            return argv[++i];
        };

        if (opt == "-h" || opt == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (opt == "--version") {
            std::cout << "release_ftp_cli 1.0.0\n";
            return 0;
        }
        if (opt == "-i" || opt == "--interactive") {
            cfg.interactive = true;
            continue;
        }
        if (opt == "--force-active") {
            cfg.data_mode = yuan::net::ftp::FtpClient::DataMode::active_only;
            continue;
        }
        if (opt == "--force-passive") {
            cfg.data_mode = yuan::net::ftp::FtpClient::DataMode::passive_only;
            continue;
        }
        if (opt == "--host") {
            cfg.host = need_value(opt);
            continue;
        }
        if (opt == "-p" || opt == "--port") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_int_value(value, cfg.port)) {
                std::cerr << "invalid port: " << value << '\n';
                return 2;
            }
            continue;
        }
        if (opt == "-u" || opt == "--user") {
            cfg.username = need_value(opt);
            continue;
        }
        if (opt == "--password") {
            cfg.password = need_value(opt);
            continue;
        }
        if (opt == "--list") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.list_path = argv[++i];
            }
            continue;
        }
        if (opt == "--nlist") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.nlist_path = argv[++i];
            }
            continue;
        }
        if (opt == "--download") {
            cfg.download_remote = need_value(opt);
            cfg.download_local = need_value(opt);
            if (cfg.download_remote.empty() || cfg.download_local.empty()) {
                return 2;
            }
            continue;
        }
        if (opt == "--upload") {
            cfg.upload_local = need_value(opt);
            cfg.upload_remote = need_value(opt);
            if (cfg.upload_local.empty() || cfg.upload_remote.empty()) {
                return 2;
            }
            continue;
        }
        if (opt == "--append") {
            cfg.append_local = need_value(opt);
            cfg.append_remote = need_value(opt);
            if (cfg.append_local.empty() || cfg.append_remote.empty()) {
                return 2;
            }
            continue;
        }

        std::cerr << "unknown option: " << opt << '\n';
        print_usage(argv[0]);
        return 2;
    }

    if (cfg.port <= 0 || cfg.port > 65535) {
        std::cerr << "port out of range: " << cfg.port << '\n';
        return 2;
    }

    yuan::net::ftp::FtpClient client;
    client.set_data_mode(cfg.data_mode);
    if (!client.connect(cfg.host, static_cast<uint16_t>(cfg.port))) {
        std::cerr << "connect failed: " << cfg.host << ":" << cfg.port << '\n';
        return 1;
    }
    if (!client.login(cfg.username, cfg.password)) {
        std::cerr << "login failed for user: " << cfg.username << '\n';
        return 1;
    }

    bool ok = true;

    if (cfg.interactive) {
        run_interactive(client);
    }

    if (!cfg.list_path.empty() || (!cfg.interactive && cfg.nlist_path.empty() && cfg.download_remote.empty() &&
                                   cfg.upload_local.empty() && cfg.append_local.empty())) {
        std::cout << client.list(cfg.list_path);
    }
    if (!cfg.nlist_path.empty()) {
        std::cout << client.nlist(cfg.nlist_path);
    }
    if (!cfg.download_remote.empty()) {
        ok = client.download(cfg.download_remote, cfg.download_local) && ok;
    }
    if (!cfg.upload_local.empty()) {
        if (!std::filesystem::exists(cfg.upload_local)) {
            std::cerr << "upload source does not exist: " << cfg.upload_local << '\n';
            ok = false;
        } else {
            ok = client.upload(cfg.upload_local, cfg.upload_remote) && ok;
        }
    }
    if (!cfg.append_local.empty()) {
        if (!std::filesystem::exists(cfg.append_local)) {
            std::cerr << "append source does not exist: " << cfg.append_local << '\n';
            ok = false;
        } else {
            ok = client.append(cfg.append_local, cfg.append_remote) && ok;
        }
    }

    client.quit();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return ok ? 0 : 1;
}
