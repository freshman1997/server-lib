#ifndef __NET_SSH_SFTP_SSH_SFTP_CODEC_H__
#define __NET_SSH_SFTP_SSH_SFTP_CODEC_H__

#include "buffer/byte_buffer.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    using ::yuan::buffer::ByteBuffer;

    enum class SftpPacketType : uint8_t {
        SSH_FXP_INIT = 1,
        SSH_FXP_VERSION = 2,
        SSH_FXP_OPEN = 3,
        SSH_FXP_CLOSE = 4,
        SSH_FXP_READ = 5,
        SSH_FXP_WRITE = 6,
        SSH_FXP_LSTAT = 7,
        SSH_FXP_FSTAT = 8,
        SSH_FXP_SETSTAT = 9,
        SSH_FXP_FSETSTAT = 10,
        SSH_FXP_OPENDIR = 11,
        SSH_FXP_READDIR = 12,
        SSH_FXP_REMOVE = 13,
        SSH_FXP_MKDIR = 14,
        SSH_FXP_RMDIR = 15,
        SSH_FXP_REALPATH = 16,
        SSH_FXP_STAT = 17,
        SSH_FXP_RENAME = 18,
        SSH_FXP_READLINK = 19,
        SSH_FXP_SYMLINK = 20,
        SSH_FXP_STATUS = 101,
        SSH_FXP_HANDLE = 102,
        SSH_FXP_DATA = 103,
        SSH_FXP_NAME = 104,
        SSH_FXP_ATTRS = 105,
        SSH_FXP_EXTENDED = 200,
        SSH_FXP_EXTENDED_REPLY = 201
    };

    enum class SftpOpenFlags : uint32_t {
        SSH_FXF_READ = 0x00000001,
        SSH_FXF_WRITE = 0x00000002,
        SSH_FXF_APPEND = 0x00000004,
        SSH_FXF_CREAT = 0x00000008,
        SSH_FXF_TRUNC = 0x00000010,
        SSH_FXF_EXCL = 0x00000020
    };

    enum class SftpStatus : uint32_t {
        SSH_FX_OK = 0,
        SSH_FX_EOF = 1,
        SSH_FX_NO_SUCH_FILE = 2,
        SSH_FX_PERMISSION_DENIED = 3,
        SSH_FX_FAILURE = 4,
        SSH_FX_BAD_MESSAGE = 5,
        SSH_FX_NO_CONNECTION = 6,
        SSH_FX_CONNECTION_LOST = 7,
        SSH_FX_OP_UNSUPPORTED = 8,
        SSH_FX_INVALID_HANDLE = 9,
        SSH_FX_NO_SUCH_PATH = 10,
        SSH_FX_FILE_ALREADY_EXISTS = 11,
        SSH_FX_WRITE_PROTECT = 12,
        SSH_FX_NO_MEDIA = 13,
        SSH_FX_NO_SPACE_ON_FILESYSTEM = 14,
        SSH_FX_QUOTA_EXCEEDED = 15,
        SSH_FX_UNKNOWN_PRINCIPAL = 16,
        SSH_FX_LOCK_CONFLICT = 17,
        SSH_FX_DIR_NOT_EMPTY = 18,
        SSH_FX_NOT_A_DIRECTORY = 19,
        SSH_FX_INVALID_FILENAME = 20,
        SSH_FX_LINK_LOOP = 21
    };

    enum class SftpRenameFlags : uint32_t {
        SSH_FXP_RENAME_OVERWRITE = 0x00000001,
        SSH_FXP_RENAME_ATOMIC = 0x00000002,
        SSH_FXP_RENAME_NATIVE = 0x00000004
    };

    constexpr uint32_t SSH_FILEXFER_ATTR_SIZE = 0x00000001;
    constexpr uint32_t SSH_FILEXFER_ATTR_UIDGID = 0x00000002;
    constexpr uint32_t SSH_FILEXFER_ATTR_PERMISSIONS = 0x00000004;
    constexpr uint32_t SSH_FILEXFER_ATTR_ACMODTIME = 0x00000008;
    constexpr uint32_t SSH_FILEXFER_ATTR_EXTENDED = 0x80000000;

    constexpr uint32_t SFTP_DEFAULT_VERSION = 3;
    constexpr uint32_t SFTP_MAX_READ_SIZE = 32768;
    constexpr size_t SFTP_MAX_HANDLE_COUNT = 256;

    struct SftpFileAttrs
    {
        uint32_t flags = 0;
        uint64_t size = 0;
        uint32_t uid = 0;
        uint32_t gid = 0;
        uint32_t permissions = 0;
        uint32_t atime = 0;
        uint32_t mtime = 0;
        std::vector<std::string> extended_names;
        std::vector<std::string> extended_values;
    };

    struct SftpNameEntry
    {
        std::string filename;
        std::string longname;
        SftpFileAttrs attrs;
    };

    struct SftpPacket
    {
        SftpPacketType type = SftpPacketType::SSH_FXP_INIT;
        uint32_t request_id = 0;
        std::vector<uint8_t> payload;
    };

    class SshSftpCodec
    {
    public:
        static std::optional<SftpPacket> decode(const uint8_t *data, size_t len);
        static ByteBuffer encode(const SftpPacket &packet);

        static ByteBuffer encode_version(uint32_t version);
        static ByteBuffer encode_status(uint32_t request_id, SftpStatus code,
                                        const std::string &message, const std::string &language = "en");
        static ByteBuffer encode_handle(uint32_t request_id, const std::string &handle);
        static ByteBuffer encode_data(uint32_t request_id, const uint8_t *data, size_t len);
        static ByteBuffer encode_name(uint32_t request_id, const std::vector<SftpNameEntry> &entries);
        static ByteBuffer encode_attrs(uint32_t request_id, const SftpFileAttrs &attrs);

        static void encode_attrs_fields(ByteBuffer &buf, const SftpFileAttrs &attrs);
        static std::optional<SftpFileAttrs> decode_attrs(const uint8_t *data, size_t len, size_t &offset);

        static std::optional<std::string> read_handle(const uint8_t *data, size_t len, size_t &offset);
    };
}

#endif
