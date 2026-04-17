#ifndef __NET_SMB_SMB_CONFIG_H__
#define __NET_SMB_SMB_CONFIG_H__

#include <cstdint>
#include <string>
#include <vector>
#include "protocol/smb2_constants.h"

namespace yuan::net::smb
{
    struct SmbShareConfig
    {
        std::string name;
        std::string comment;
        ShareType type = ShareType::DISK;
        std::string path;
        std::string password;
        int max_uses = -1;
        uint32_t share_flags = 0;
        uint32_t capabilities = 0;
    };

    struct SmbServerConfig
    {
        uint16_t port = 445;
        std::string server_name = "YUAN-SMB";
        std::string domain_name = "WORKGROUP";
        std::string server_comment = "server-lib SMB Server";
        bool enable_smb1_fallback = true;
        bool enable_encryption = false;
        bool require_signing = false;
        uint32_t max_sessions = 1024;
        uint16_t max_credits = 512;
        uint32_t idle_timeout_ms = 300000;
        uint32_t max_transact_size = 1048576;
        uint32_t max_read_size = 1048576;
        uint32_t max_write_size = 1048576;
        std::vector<DialectRevision> supported_dialects = {
            DialectRevision::SMB_2_002,
            DialectRevision::SMB_2_1,
            DialectRevision::SMB_3_0,
            DialectRevision::SMB_3_0_2,
            DialectRevision::SMB_3_1_1
        };
        std::vector<SmbShareConfig> shares;
    };
}
#endif
