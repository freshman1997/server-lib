#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{

std::string shell_quote(const std::string &value)
{
#ifdef _WIN32
    return "\"" + value + "\"";
#else
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out += "'";
    return out;
#endif
}

int run_command(const std::string &command)
{
    return std::system(command.c_str());
}

std::filesystem::path find_release_dns_binary()
{
    const auto cwd = std::filesystem::current_path();

#ifdef _WIN32
    const std::string binary_name = "release_dns_server.exe";
#else
    const std::string binary_name = "release_dns_server";
#endif

    const auto candidate_from_build = cwd / "release" / "dns" / binary_name;
    if (std::filesystem::exists(candidate_from_build)) {
        return candidate_from_build;
    }

    const auto candidate_from_repo = cwd / "build" / "release" / "dns" / binary_name;
    if (std::filesystem::exists(candidate_from_repo)) {
        return candidate_from_repo;
    }

    return {};
}

} // namespace

int main()
{
    const char *smoke_env = std::getenv("YUAN_RUN_RELEASE_DNS_SMOKE");
    if (!smoke_env || std::string(smoke_env) != "1") {
        std::cout << "release_dns_server smoke skipped (set YUAN_RUN_RELEASE_DNS_SMOKE=1 to enable)." << std::endl;
        return 0;
    }

    const auto server_bin = find_release_dns_binary();
    if (server_bin.empty()) {
        std::cout << "release_dns_server binary not found; skipping smoke." << std::endl;
        return 0;
    }

    const std::string base = shell_quote(server_bin.string());

    if (run_command(base + " --help") != 0) {
        std::cerr << "release_dns_server --help failed" << std::endl;
        return 1;
    }

    if (run_command(base + " --version") != 0) {
        std::cerr << "release_dns_server --version failed" << std::endl;
        return 1;
    }

    if (run_command(base + " --unknown-option") == 0) {
        std::cerr << "release_dns_server unknown option should fail" << std::endl;
        return 1;
    }

    std::cout << "release_dns_server smoke passed." << std::endl;
    return 0;
}
