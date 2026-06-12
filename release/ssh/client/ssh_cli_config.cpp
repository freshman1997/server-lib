#include "ssh_cli_config.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace yuan::release_ssh::client
{
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

    std::string read_env_string(const char *name, const std::string &default_value)
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

    std::string to_lower_ascii(std::string value)
    {
        for (char &ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    bool parse_host_key_policy_value(const std::string &value, CliArgs::HostKeyPolicy &policy_out)
    {
        const auto value_lc = to_lower_ascii(value);
        if (value_lc == "yes" || value_lc == "true" || value_lc == "on") {
            policy_out = CliArgs::HostKeyPolicy::yes;
            return true;
        }
        if (value_lc == "accept-new") {
            policy_out = CliArgs::HostKeyPolicy::accept_new;
            return true;
        }
        if (value_lc == "no" || value_lc == "false" || value_lc == "off") {
            policy_out = CliArgs::HostKeyPolicy::no;
            return true;
        }
        return false;
    }

    std::string trim_copy(const std::string &value)
    {
        size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
            ++begin;
        }
        size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    std::vector<std::string> split_ws(const std::string &line)
    {
        std::vector<std::string> parts;
        std::istringstream iss(line);
        std::string token;
        while (iss >> token) {
            parts.push_back(std::move(token));
        }
        return parts;
    }

    bool host_matches_known_hosts_field(const std::string &host_field,
                                        const std::string &expected_host_token,
                                        const std::string &expected_host_plain)
    {
        size_t start = 0;
        while (start <= host_field.size()) {
            const size_t comma = host_field.find(',', start);
            const size_t end = comma == std::string::npos ? host_field.size() : comma;
            const std::string candidate = host_field.substr(start, end - start);
            if (candidate == expected_host_token || candidate == expected_host_plain) {
                return true;
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        return false;
    }

    std::string default_known_hosts_file()
    {
        const std::string home = read_env_string("HOME");
        if (!home.empty()) {
            return (std::filesystem::path(home) / ".ssh" / "known_hosts").string();
        }
#ifdef _WIN32
        const std::string user_profile = read_env_string("USERPROFILE");
        if (!user_profile.empty()) {
            return (std::filesystem::path(user_profile) / ".ssh" / "known_hosts").string();
        }
        const std::string home_drive = read_env_string("HOMEDRIVE");
        const std::string home_path = read_env_string("HOMEPATH");
        if (!home_drive.empty() && !home_path.empty()) {
            return (std::filesystem::path(home_drive + home_path) / ".ssh" / "known_hosts").string();
        }
#endif
        return "known_hosts";
    }

    void print_usage(const char *program)
    {
        std::cout
            << "release_ssh_cli\n"
            << "usage:\n"
            << "  " << program << " [options] [user@]host [command]\n"
            << "  " << program << " --probe -p 2222 yuan@127.0.0.1\n\n"
            << "options:\n"
            << "  -p <port>                 Port to connect to\n"
            << "  -l <login_name>           Login username\n"
            << "  -i <identity_file>        Private key file for publickey auth\n"
            << "  -L <[bind_addr:]port:host:hostport> Local forward rule\n"
            << "  -D <[bind_addr:]port>     Dynamic SOCKS5 forward\n"
            << "  -R <[bind_addr:]port:host:hostport> Remote forward rule\n"
            << "  -o <key=value>            OpenSSH-style option override\n"
            << "  -F <config_file>          Record config file option\n"
            << "      --known-hosts <path>  Known hosts path (default ~/.ssh/known_hosts)\n"
            << "      --strict-host-key-checking <yes|accept-new|no> Host key policy\n"
            << "  -q                        Quiet mode\n"
            << "  -v                        Verbose mode, repeatable\n"
            << "  -V, --version             Print version\n"
            << "  -h, --help                Show this help\n"
            << "      --host <host>         Explicit host, for script compatibility\n"
            << "      --port <port>         Explicit port, for script compatibility\n"
            << "      --user <user>         Explicit user, for script compatibility\n"
            << "      --password <password> Password auth secret (or YUAN_SSH_PASSWORD)\n"
            << "      --command <cmd>       Command to execute\n"
            << "      --timeout-ms <ms>     Socket receive timeout\n"
            << "      --stderr-prefix       Prefix stderr lines with [stderr]\n"
            << "      --probe               Only verify TCP + SSH version exchange\n\n"
            << "env defaults:\n"
            << "  YUAN_SSH_HOST\n"
            << "  YUAN_SSH_PORT\n"
            << "  YUAN_SSH_TIMEOUT_MS\n"
            << "  YUAN_SSH_USER\n"
            << "  YUAN_SSH_PASSWORD\n"
            << "  YUAN_SSH_COMMAND\n";
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

    void apply_option(CliArgs &args, const std::string &raw)
    {
        const auto eq = raw.find('=');
        const std::string key = eq == std::string::npos ? raw : raw.substr(0, eq);
        const std::string value = eq == std::string::npos ? "yes" : raw.substr(eq + 1);
        const auto key_lc = to_lower_ascii(key);
        args.options[key] = value;
        if (key_lc == "port") {
            (void)parse_int_value(value, args.port);
        } else if (key_lc == "user") {
            args.user = value;
        } else if (key_lc == "batchmode") {
            args.batch_mode = value == "yes" || value == "true" || value == "1";
        } else if (key_lc == "connecttimeout") {
            int seconds = 0;
            if (parse_int_value(value, seconds) && seconds > 0) {
                args.timeout_ms = seconds * 1000;
            }
        } else if (key_lc == "stricthostkeychecking") {
            (void)parse_host_key_policy_value(value, args.host_key_policy);
        } else if (key_lc == "userknownhostsfile") {
            args.known_hosts_file = value;
        }
    }

    void parse_destination(const std::string &destination, CliArgs &args)
    {
        const auto at = destination.find('@');
        if (at != std::string::npos) {
            args.user = destination.substr(0, at);
            args.host = destination.substr(at + 1);
        } else {
            args.host = destination;
        }
    }

    bool parse_args(int argc, char **argv, CliArgs &args)
    {
        args.host = read_env_string("YUAN_SSH_HOST", args.host);
        args.port = read_env_int("YUAN_SSH_PORT", args.port);
        args.user = read_env_string("YUAN_SSH_USER", args.user);
        args.password = read_env_string("YUAN_SSH_PASSWORD", args.password);
        args.command = read_env_string("YUAN_SSH_COMMAND", args.command);
        args.timeout_ms = read_env_int("YUAN_SSH_TIMEOUT_MS", args.timeout_ms);
        args.known_hosts_file = read_env_string("YUAN_SSH_KNOWN_HOSTS", args.known_hosts_file);

        bool destination_seen = false;
        for (int i = 1; i < argc; ++i) {
            const std::string opt = argv[i];
            if (destination_seen) {
                std::ostringstream command;
                command << opt;
                for (++i; i < argc; ++i) {
                    command << ' ' << argv[i];
                }
                args.command = command.str();
                break;
            }
            auto need_value = [&](const std::string &name) -> std::string {
                if (i + 1 >= argc) {
                    std::cerr << "missing value for " << name << '\n';
                    return {};
                }
                return argv[++i];
            };

            if (opt == "-h" || opt == "--help") {
                args.help = true;
                return true;
            }
            if (opt == "-V" || opt == "--version") {
                args.version = true;
                return true;
            }
            if (opt == "--") {
                if (i + 1 < argc && args.host.empty()) {
                    parse_destination(argv[++i], args);
                }
                if (i + 1 < argc) {
                    std::ostringstream command;
                    command << argv[++i];
                    for (++i; i < argc; ++i) {
                        command << ' ' << argv[i];
                    }
                    args.command = command.str();
                }
                break;
            }
            if (opt == "--host") {
                args.host = need_value(opt);
            } else if (opt == "-p" || opt == "--port") {
                const auto value = need_value(opt);
                if (value.empty() || !parse_int_value(value, args.port)) {
                    std::cerr << "invalid port: " << value << '\n';
                    return false;
                }
            } else if (opt == "-l" || opt == "--user") {
                args.user = need_value(opt);
            } else if (opt == "--password") {
                args.password = need_value(opt);
            } else if (opt == "--command") {
                args.command = need_value(opt);
            } else if (opt == "-i") {
                auto value = need_value(opt);
                if (value.empty()) {
                    return false;
                }
                args.identity_files.push_back(std::move(value));
            } else if (opt == "-o") {
                const auto value = need_value(opt);
                if (value.empty()) {
                    return false;
                }
                apply_option(args, value);
            } else if (opt == "-F") {
                args.config_file = need_value(opt);
            } else if (opt == "-L") {
                auto value = need_value(opt);
                if (value.empty()) return false;
                args.local_forwards.push_back(std::move(value));
            } else if (opt == "-D") {
                auto value = need_value(opt);
                if (value.empty()) return false;
                args.dynamic_forwards.push_back(std::move(value));
            } else if (opt == "-R") {
                auto value = need_value(opt);
                if (value.empty()) return false;
                args.remote_forwards.push_back(std::move(value));
            } else if (opt == "--known-hosts") {
                args.known_hosts_file = need_value(opt);
            } else if (opt == "--strict-host-key-checking") {
                const auto value = need_value(opt);
                if (!parse_host_key_policy_value(value, args.host_key_policy)) {
                    std::cerr << "invalid strict host key policy: " << value << '\n';
                    return false;
                }
            } else if (opt == "--timeout-ms") {
                const auto value = need_value(opt);
                if (!parse_int_value(value, args.timeout_ms)) {
                    std::cerr << "invalid timeout: " << value << '\n';
                    return false;
                }
            } else if (opt == "--probe") {
                args.probe = true;
            } else if (opt == "--stderr-prefix") {
                args.stderr_prefix = true;
            } else if (opt == "-q" || opt == "--quiet") {
                args.quiet = true;
            } else if (opt == "-v") {
                ++args.verbose;
            } else if (!opt.empty() && opt[0] == '-' && opt.find_first_not_of('v', 1) == std::string::npos) {
                args.verbose += static_cast<int>(opt.size() - 1);
            } else if (!destination_seen) {
                parse_destination(opt, args);
                destination_seen = true;
            } else {
                std::ostringstream command;
                command << opt;
                for (++i; i < argc; ++i) {
                    command << ' ' << argv[i];
                }
                args.command = command.str();
                break;
            }
        }

        if (args.timeout_ms < 100) {
            args.timeout_ms = 100;
        }
        if (args.host.empty() || args.port <= 0 || args.port > 65535) {
            std::cerr << "invalid host/port\n";
            return false;
        }
        if (args.probe) {
            return true;
        }
        if (args.user.empty()) {
            std::cerr << "--user is required\n";
            return false;
        }
        if (!args.identity_files.empty() && args.identity_files.size() > 1) {
            std::cerr << "multiple -i identity files are not supported yet\n";
            return false;
        }
        if (args.password.empty() && args.identity_files.empty()) {
            std::cerr << "either --password (or YUAN_SSH_PASSWORD) or -i <identity_file> is required\n";
            return false;
        }
        return true;
    }
}
