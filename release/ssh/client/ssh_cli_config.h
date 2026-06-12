#ifndef YUAN_RELEASE_SSH_CLI_CONFIG_H
#define YUAN_RELEASE_SSH_CLI_CONFIG_H

#include <map>
#include <string>
#include <vector>

namespace yuan::release_ssh::client
{
    struct CliArgs
    {
        enum class HostKeyPolicy {
            no,
            accept_new,
            yes
        };

        std::string host;
        int port = 22;
        int timeout_ms = 5000;
        std::string user;
        std::string password;
        std::string command;
        std::string config_file;
        std::vector<std::string> local_forwards;
        std::vector<std::string> dynamic_forwards;
        std::vector<std::string> remote_forwards;
        std::vector<std::string> identity_files;
        std::map<std::string, std::string> options;
        bool batch_mode = false;
        bool quiet = false;
        int verbose = 0;
        bool help = false;
        bool version = false;
        bool probe = false;
        bool stderr_prefix = false;
        HostKeyPolicy host_key_policy = HostKeyPolicy::no;
        std::string known_hosts_file;
    };

    int read_env_int(const char *name, int default_value);
    std::string read_env_string(const char *name, const std::string &default_value = {});
    std::string to_lower_ascii(std::string value);
    bool parse_host_key_policy_value(const std::string &value, CliArgs::HostKeyPolicy &policy_out);
    std::string trim_copy(const std::string &value);
    std::vector<std::string> split_ws(const std::string &line);
    bool host_matches_known_hosts_field(const std::string &host_field,
                                        const std::string &expected_host_token,
                                        const std::string &expected_host_plain);
    std::string default_known_hosts_file();
    void print_usage(const char *program);
    bool parse_int_value(const std::string &raw, int &out);
    void apply_option(CliArgs &args, const std::string &raw);
    void parse_destination(const std::string &destination, CliArgs &args);
    bool parse_args(int argc, char **argv, CliArgs &args);
}

#endif
