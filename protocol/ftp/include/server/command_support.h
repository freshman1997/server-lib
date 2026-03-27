#ifndef __NET_FTP_SERVER_COMMAND_SUPPORT_H__
#define __NET_FTP_SERVER_COMMAND_SUPPORT_H__

#include "common/def.h"
#include <filesystem>
#include <string>
#include <vector>

namespace yuan::net::ftp
{
    class FtpSession;

    bool ensure_login(FtpSession *session, FtpCommandResponse &response);
    std::filesystem::path resolve_path(FtpSession *session, const std::string &pathArg);
    bool path_within_root(FtpSession *session, const std::filesystem::path &path);
    std::string to_virtual_path(FtpSession *session, const std::filesystem::path &path);
    std::string build_pasv_response(const std::string &ip, int port);
    std::vector<std::string> build_list_lines(const std::filesystem::path &path);
    std::string build_list_payload(const std::filesystem::path &path);
    std::string build_nlist_payload(const std::filesystem::path &path);
}

#endif
