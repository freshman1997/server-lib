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

std::filesystem::path find_release_webrtc_binary()
{
    const auto cwd = std::filesystem::current_path();

#ifdef _WIN32
    const std::string binary_name = "release_webrtc_server.exe";
#else
    const std::string binary_name = "release_webrtc_server";
#endif

    const auto candidate_from_build = cwd / "release" / "webrtc" / binary_name;
    if (std::filesystem::exists(candidate_from_build)) {
        return candidate_from_build;
    }

    const auto candidate_from_repo = cwd / "build" / "release" / "webrtc" / binary_name;
    if (std::filesystem::exists(candidate_from_repo)) {
        return candidate_from_repo;
    }

    const auto candidate_from_repo_mingw = cwd / "build-mingw" / "release" / "webrtc" / binary_name;
    if (std::filesystem::exists(candidate_from_repo_mingw)) {
        return candidate_from_repo_mingw;
    }

    return {};
}

} // namespace

int main()
{
    const char *smoke_env = std::getenv("YUAN_RUN_RELEASE_WEBRTC_SMOKE");
    if (!smoke_env || std::string(smoke_env) != "1") {
        std::cout << "release_webrtc_server smoke skipped (set YUAN_RUN_RELEASE_WEBRTC_SMOKE=1 to enable)." << std::endl;
        return 0;
    }

    const auto server_bin = find_release_webrtc_binary();
    if (server_bin.empty()) {
        std::cout << "release_webrtc_server binary not found; skipping smoke." << std::endl;
        return 0;
    }

    const std::string base = shell_quote(server_bin.string());

    if (run_command(base + " --help") != 0) {
        std::cerr << "release_webrtc_server --help failed" << std::endl;
        return 1;
    }

    if (run_command(base + " --version") != 0) {
        std::cerr << "release_webrtc_server --version failed" << std::endl;
        return 1;
    }

    if (run_command(base + " --unknown-option") == 0) {
        std::cerr << "release_webrtc_server unknown option should fail" << std::endl;
        return 1;
    }

    if (run_command(base + " --self-check-only --probe-host 127.0.0.1 --port 65535") == 0) {
        std::cerr << "release_webrtc_server self-check should fail when no server is running" << std::endl;
        return 1;
    }

    std::cout << "release_webrtc_server smoke passed." << std::endl;
    return 0;
}
