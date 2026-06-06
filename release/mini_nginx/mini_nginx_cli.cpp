#include "mini_nginx_common.h"

namespace mini_nginx
{
    void print_usage(const char *program)
    {
        std::cout << "mini_nginx usage:\n"
                  << "  " << program << " [config.json]\n\n"
                  << "  " << program << " -c config.json\n"
                  << "  " << program << " -t -c config.json\n\n"
                  << "options:\n"
                  << "  -c, --config <path>   use a JSON config file\n"
                  << "  -t, --test           test config and exit without starting\n"
                  << "  -h, --help           show this help\n\n"
                  << "env overrides:\n"
                  << "  YUAN_MINI_NGINX_PORT\n"
                  << "  YUAN_MINI_NGINX_SERVER_NAME\n"
                  << "  YUAN_MINI_NGINX_WORKERS\n"
                  << "  YUAN_MINI_NGINX_USE_IOCP\n"
                  << "  YUAN_MINI_NGINX_ACCESS_LOG\n"
                  << "  YUAN_MINI_NGINX_ACCESS_LOG_PATH\n";
    }

    bool parse_cli(int argc, char **argv, CliOptions &options)
    {
        bool saw_config = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i] ? argv[i] : "";
            if (arg == "-h" || arg == "--help") {
                options.help = true;
                return true;
            }
            if (arg == "-t" || arg == "--test") {
                options.test_config = true;
                continue;
            }
            if (arg == "-c" || arg == "--config") {
                if (i + 1 >= argc || !argv[i + 1] || std::string(argv[i + 1]).empty()) {
                    std::cerr << arg << " requires a config path\n";
                    return false;
                }
                options.config_path = argv[++i];
                saw_config = true;
                continue;
            }
            const std::string config_prefix = "--config=";
            if (arg.rfind(config_prefix, 0) == 0) {
                const auto path = arg.substr(config_prefix.size());
                if (path.empty()) {
                    std::cerr << "--config requires a config path\n";
                    return false;
                }
                options.config_path = path;
                saw_config = true;
                continue;
            }
            if (!arg.empty() && arg.front() == '-') {
                std::cerr << "unknown option: " << arg << '\n';
                return false;
            }
            if (saw_config) {
                std::cerr << "unexpected extra argument: " << arg << '\n';
                return false;
            }
            options.config_path = arg;
            saw_config = true;
        }
        return true;
    }
}
