#include "server/command_support.h"
#include "common/session.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace yuan::net::ftp
{
    namespace
    {
        namespace fs = std::filesystem;
        fs::path session_root(FtpSession *session) { return fs::path(session->get_root_dir()).lexically_normal(); }
        std::string permission_string(const fs::directory_entry &entry)
        {
            const auto perms = entry.status().permissions();
            std::string value;
            value += entry.is_directory() ? 'd' : '-';
            value += (perms & fs::perms::owner_read) != fs::perms::none ? 'r' : '-';
            value += (perms & fs::perms::owner_write) != fs::perms::none ? 'w' : '-';
            value += (perms & fs::perms::owner_exec) != fs::perms::none ? 'x' : '-';
            value += (perms & fs::perms::group_read) != fs::perms::none ? 'r' : '-';
            value += (perms & fs::perms::group_write) != fs::perms::none ? 'w' : '-';
            value += (perms & fs::perms::group_exec) != fs::perms::none ? 'x' : '-';
            value += (perms & fs::perms::others_read) != fs::perms::none ? 'r' : '-';
            value += (perms & fs::perms::others_write) != fs::perms::none ? 'w' : '-';
            value += (perms & fs::perms::others_exec) != fs::perms::none ? 'x' : '-';
            return value;
        }
    }

    bool ensure_login(FtpSession *session, FtpCommandResponse &response)
    {
        if (session && session->login_success()) {
            return true;
        }
        response = {FtpResponseCode::__530__, "Please login with USER and PASS."};
        return false;
    }

    fs::path resolve_path(FtpSession *session, const std::string &pathArg)
    {
        const fs::path root = session_root(session);
        if (pathArg.empty()) {
            return (root / fs::path(session->get_cwd()).relative_path()).lexically_normal();
        }
        fs::path request(pathArg);
        return (request.is_absolute() ? (root / request.relative_path()) : (root / fs::path(session->get_cwd()).relative_path() / request)).lexically_normal();
    }

    bool path_within_root(FtpSession *session, const fs::path &path)
    {
        const auto root = session_root(session).generic_string();
        const auto candidate = path.lexically_normal().generic_string();
        return candidate.size() >= root.size() && candidate.compare(0, root.size(), root) == 0;
    }

    std::string to_virtual_path(FtpSession *session, const fs::path &path)
    {
        auto relative = path.lexically_normal().lexically_relative(session_root(session));
        if (relative.empty() || relative == ".") {
            return "/";
        }
        auto virtualPath = (fs::path("/") / relative).lexically_normal().generic_string();
        return virtualPath.empty() ? "/" : virtualPath;
    }

    std::string build_pasv_response(const std::string &ip, int port)
    {
        std::stringstream ss(ip);
        std::string item;
        std::vector<std::string> segs;
        while (std::getline(ss, item, '.')) {
            segs.push_back(item);
        }
        while (segs.size() < 4) {
            segs.push_back("0");
        }
        std::ostringstream out;
        out << "Entering Passive Mode (" << segs[0] << ',' << segs[1] << ',' << segs[2] << ',' << segs[3] << ',' << (port / 256) << ',' << (port % 256) << ')';
        return out.str();
    }

    std::vector<std::string> build_list_lines(const fs::path &path)
    {
        std::vector<std::string> lines;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return lines;
        }
        auto appendLine = [&](const fs::directory_entry &entry) {
            std::ostringstream out;
            out << permission_string(entry) << " 1 owner group " << (entry.is_regular_file() ? entry.file_size() : 0) << ' ';
            auto time = entry.last_write_time();
            auto mapped = std::chrono::time_point_cast<std::chrono::system_clock::duration>(time - decltype(time)::clock::now() + std::chrono::system_clock::now());
            auto tt = std::chrono::system_clock::to_time_t(mapped);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            out << std::put_time(&tm, "%b %d %H:%M") << ' ' << entry.path().filename().string();
            lines.push_back(out.str());
        };
        if (fs::is_regular_file(path, ec)) {
            appendLine(fs::directory_entry(path));
            return lines;
        }
        for (const auto &entry : fs::directory_iterator(path, ec)) {
            appendLine(entry);
        }
        return lines;
    }

    std::string build_list_payload(const fs::path &path)
    {
        std::string payload;
        for (const auto &line : build_list_lines(path)) {
            payload += line;
            payload += "\r\n";
        }
        return payload;
    }

    std::string build_nlist_payload(const fs::path &path)
    {
        namespace fs = std::filesystem;
        std::string payload;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return payload;
        }
        if (fs::is_regular_file(path, ec)) {
            payload += path.filename().string();
            payload += "\r\n";
            return payload;
        }
        for (const auto &entry : fs::directory_iterator(path, ec)) {
            payload += entry.path().filename().string();
            payload += "\r\n";
        }
        return payload;
    }
}
