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

std::filesystem::path find_repo_root()
{
    auto current = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i) {
        if (std::filesystem::exists(current / "release" / "dns")) {
            return current;
        }
        if (!current.has_parent_path()) {
            break;
        }
        current = current.parent_path();
    }
    return {};
}

} // namespace

int main()
{
    const char *gate_env = std::getenv("YUAN_RUN_RELEASE_DNS_GATE");
    if (!gate_env || std::string(gate_env) != "1") {
        std::cout << "release_dns gate skipped (set YUAN_RUN_RELEASE_DNS_GATE=1 to enable)." << std::endl;
        return 0;
    }

    const auto repo_root = find_repo_root();
    if (repo_root.empty()) {
        std::cout << "repo root not found; skipping release_dns gate." << std::endl;
        return 0;
    }

    const auto build_dir = std::filesystem::current_path();

#ifdef _WIN32
    const auto gate_script = repo_root / "release" / "dns" / "gate.ps1";
    if (!std::filesystem::exists(gate_script)) {
        std::cerr << "gate script not found: " << gate_script.string() << std::endl;
        return 1;
    }

    const std::string cmd =
        "pwsh -File " + shell_quote(gate_script.string()) +
        " -BuildDir " + shell_quote(build_dir.string()) +
        " -DnsPort 5353";
    if (run_command(cmd) != 0) {
        std::cerr << "release_dns gate failed" << std::endl;
        return 1;
    }
#else
    const auto gate_script = repo_root / "release" / "dns" / "gate.sh";
    if (!std::filesystem::exists(gate_script)) {
        std::cerr << "gate script not found: " << gate_script.string() << std::endl;
        return 1;
    }

    const std::string cmd =
        "BUILD_DIR=" + shell_quote(build_dir.string()) +
        " bash " + shell_quote(gate_script.string());
    if (run_command(cmd) != 0) {
        std::cerr << "release_dns gate failed" << std::endl;
        return 1;
    }
#endif

    std::cout << "release_dns gate passed." << std::endl;
    return 0;
}
