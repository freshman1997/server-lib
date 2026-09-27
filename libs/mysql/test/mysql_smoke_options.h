#ifndef YUAN_MYSQL_TEST_MYSQL_SMOKE_OPTIONS_H
#define YUAN_MYSQL_TEST_MYSQL_SMOKE_OPTIONS_H

#include "yuan/mysql/mysql_client.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace yuan::mysql::test
{
    inline const char *arg_value(int argc, char **argv, const char *name)
    {
        const std::string prefix = std::string(name) + "=";
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg.rfind(prefix, 0) == 0) {
                return argv[i] + prefix.size();
            }
        }
        return nullptr;
    }

    inline std::string option_or_env(int argc, char **argv, const char *arg_name, const char *env_name,
                                     std::string fallback = {})
    {
        if (const char *value = arg_value(argc, argv, arg_name)) {
            return value;
        }
        if (const char *value = std::getenv(env_name)) {
            return value;
        }
        return fallback;
    }

    inline SslMode parse_ssl_mode(const std::string &value)
    {
        if (value == "disabled") {
            return SslMode::Disabled;
        }
        if (value == "preferred") {
            return SslMode::Preferred;
        }
        if (value == "required") {
            return SslMode::Required;
        }
        if (value == "verify-ca") {
            return SslMode::VerifyCa;
        }
        if (value == "verify-identity") {
            return SslMode::VerifyIdentity;
        }
        throw std::invalid_argument("unsupported ssl mode: " + value);
    }

    inline ConnectionOptions parse_connection_options(int argc, char **argv)
    {
        ConnectionOptions options;
        options.host = option_or_env(argc, argv, "--host", "YUAN_MYSQL_HOST", options.host);
        options.user = option_or_env(argc, argv, "--user", "YUAN_MYSQL_USER");
        options.password = option_or_env(argc, argv, "--password", "YUAN_MYSQL_PASSWORD");
        options.database = option_or_env(argc, argv, "--database", "YUAN_MYSQL_DATABASE");
        options.charset = option_or_env(argc, argv, "--charset", "YUAN_MYSQL_CHARSET", "utf8mb4");
        options.ssl_mode = parse_ssl_mode(option_or_env(argc, argv, "--ssl-mode", "YUAN_MYSQL_SSL_MODE", "disabled"));

        if (std::string port = option_or_env(argc, argv, "--port", "YUAN_MYSQL_PORT"); !port.empty()) {
            options.port = static_cast<unsigned int>(std::stoul(port));
        }
        return options;
    }
}

#endif // YUAN_MYSQL_TEST_MYSQL_SMOKE_OPTIONS_H
