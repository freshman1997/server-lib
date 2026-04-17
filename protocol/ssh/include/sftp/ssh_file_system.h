#ifndef __NET_SSH_SFTP_SSH_FILE_SYSTEM_H__
#define __NET_SSH_SFTP_SSH_FILE_SYSTEM_H__

#include "sftp/ssh_sftp_codec.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    struct SshFsOpenResult
    {
        bool success = false;
        std::string handle;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    struct SshFsReadResult
    {
        bool success = false;
        std::vector<uint8_t> data;
        bool eof = false;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    struct SshFsWriteResult
    {
        bool success = false;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    struct SshFsStatResult
    {
        bool success = false;
        SftpFileAttrs attrs;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    struct SshFsReadDirResult
    {
        bool success = false;
        std::vector<SftpNameEntry> entries;
        bool eof = false;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    struct SshFsRealPathResult
    {
        bool success = false;
        std::string path;
        SftpFileAttrs attrs;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    struct SshFsReadLinkResult
    {
        bool success = false;
        std::string link_target;
        SftpFileAttrs attrs;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    struct SshFsSimpleResult
    {
        bool success = false;
        SftpStatus status = SftpStatus::SSH_FX_FAILURE;
        std::string status_message;
    };

    class SshFileSystem
    {
    public:
        virtual ~SshFileSystem() = default;

        virtual SshFsOpenResult open(const std::string &path, uint32_t pflags, const SftpFileAttrs &attrs) = 0;
        virtual SshFsSimpleResult close(const std::string &handle) = 0;
        virtual SshFsReadResult read(const std::string &handle, uint64_t offset, uint32_t len) = 0;
        virtual SshFsWriteResult write(const std::string &handle, uint64_t offset, const uint8_t *data, uint32_t len) = 0;
        virtual SshFsStatResult lstat(const std::string &path) = 0;
        virtual SshFsStatResult fstat(const std::string &handle) = 0;
        virtual SshFsSimpleResult setstat(const std::string &path, const SftpFileAttrs &attrs) = 0;
        virtual SshFsSimpleResult fsetstat(const std::string &handle, const SftpFileAttrs &attrs) = 0;
        virtual SshFsOpenResult opendir(const std::string &path) = 0;
        virtual SshFsReadDirResult readdir(const std::string &handle) = 0;
        virtual SshFsSimpleResult remove(const std::string &path) = 0;
        virtual SshFsSimpleResult mkdir(const std::string &path, const SftpFileAttrs &attrs) = 0;
        virtual SshFsSimpleResult rmdir(const std::string &path) = 0;
        virtual SshFsRealPathResult realpath(const std::string &path) = 0;
        virtual SshFsStatResult stat(const std::string &path) = 0;
        virtual SshFsSimpleResult rename(const std::string &old_path, const std::string &new_path, uint32_t flags) = 0;
        virtual SshFsReadLinkResult readlink(const std::string &path) = 0;
        virtual SshFsSimpleResult symlink(const std::string &link_path, const std::string &target_path) = 0;
    };

    class SshLocalFileSystem : public SshFileSystem
    {
    public:
        explicit SshLocalFileSystem(const std::string &root_dir);
        ~SshLocalFileSystem() override;

        SshFsOpenResult open(const std::string &path, uint32_t pflags, const SftpFileAttrs &attrs) override;
        SshFsSimpleResult close(const std::string &handle) override;
        SshFsReadResult read(const std::string &handle, uint64_t offset, uint32_t len) override;
        SshFsWriteResult write(const std::string &handle, uint64_t offset, const uint8_t *data, uint32_t len) override;
        SshFsStatResult lstat(const std::string &path) override;
        SshFsStatResult fstat(const std::string &handle) override;
        SshFsSimpleResult setstat(const std::string &path, const SftpFileAttrs &attrs) override;
        SshFsSimpleResult fsetstat(const std::string &handle, const SftpFileAttrs &attrs) override;
        SshFsOpenResult opendir(const std::string &path) override;
        SshFsReadDirResult readdir(const std::string &handle) override;
        SshFsSimpleResult remove(const std::string &path) override;
        SshFsSimpleResult mkdir(const std::string &path, const SftpFileAttrs &attrs) override;
        SshFsSimpleResult rmdir(const std::string &path) override;
        SshFsRealPathResult realpath(const std::string &path) override;
        SshFsStatResult stat(const std::string &path) override;
        SshFsSimpleResult rename(const std::string &old_path, const std::string &new_path, uint32_t flags) override;
        SshFsReadLinkResult readlink(const std::string &path) override;
        SshFsSimpleResult symlink(const std::string &link_path, const std::string &target_path) override;

    private:
        std::string resolve_path(const std::string &path) const;
        SftpFileAttrs stat_to_attrs(const struct stat &st) const;
        std::string build_longname(const std::string &filename, const struct stat &st) const;

        std::string root_dir_;
        std::unordered_map<std::string, int> file_handles_;
        std::unordered_map<std::string, void *> dir_handles_;
        uint64_t next_handle_id_ = 0;
    };
}

#endif
