#ifndef YUAN_RELEASE_SSH_SERVER_CONFIG_H
#define YUAN_RELEASE_SSH_SERVER_CONFIG_H

#include <filesystem>
#include <string>

namespace yuan::release_ssh
{
    struct ServerConfig
    {
        int port = 2222;
        std::string app_name = "release-ssh-server";
        std::string host_key_path = "hostkey/ssh_host_ed25519_key";
        std::string banner = "Welcome to Yuan SSH release server";
        std::string sftp_root_dir = ".";
        std::string software_version = "YuanSSH_Release_1.0";
        std::string username = "yuan";
        std::string password = "yuan";
        bool enable_publickey_auth = true;
        bool enable_password_auth = true;
        bool enable_sftp = true;
        bool enable_port_forwarding = true;
        int auth_failure_delay_ms = 0;
    };

    struct ServerCliArgs
    {
        std::filesystem::path config_path;
        ServerConfig overrides;
        bool has_port = false;
        bool has_host_key = false;
        bool has_user = false;
        bool has_password = false;
        bool has_banner = false;
        bool has_sftp_root = false;
        bool has_password_auth = false;
        bool has_publickey_auth = false;
        bool has_sftp = false;
        bool has_port_forward = false;
        bool has_auth_failure_delay = false;
        bool help = false;
        bool version = false;
    };

    int read_env_int(const char *name, int default_value);
    bool read_env_bool(const char *name, bool default_value);
    std::string read_env_string(const char *name, const std::string &default_value = {});
    void print_usage(const char *program);
    bool parse_bool_value(const std::string &raw, bool &out);
    bool parse_int_value(const std::string &raw, int &out);
    bool load_config_file(const std::filesystem::path &path, ServerConfig &config, std::string &error);
    std::filesystem::path default_config_path();
    bool parse_cli_args(int argc, char **argv, ServerCliArgs &args);
    void apply_cli_overrides(const ServerCliArgs &args, ServerConfig &config);
    void apply_env_overrides(ServerConfig &config);
    std::filesystem::path resolve_path_with_base(const std::string &raw_path,
                                                 const std::filesystem::path &base_dir);
    bool validate_config(const ServerConfig &config, std::string &error);
}

#endif
