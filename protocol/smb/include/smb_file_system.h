#ifndef __NET_SMB_SMB_FILE_SYSTEM_H__
#define __NET_SMB_SMB_FILE_SYSTEM_H__

#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include "buffer/byte_buffer.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    struct OpenResult
    {
        bool success = false;
        NtStatus status = NtStatus::SUCCESS;
        void *handle = nullptr;
        bool is_directory = false;
        uint32_t create_action = 0;
        uint64_t allocation_size = 0;
        uint64_t end_of_file = 0;
        uint32_t file_attributes = 0;
        uint64_t creation_time = 0;
        uint64_t last_access_time = 0;
        uint64_t last_write_time = 0;
        uint64_t change_time = 0;
    };

    struct ReadResult
    {
        bool success = false;
        NtStatus status = NtStatus::SUCCESS;
        uint32_t bytes_read = 0;
        std::vector<uint8_t> data;
    };

    struct WriteResult
    {
        bool success = false;
        NtStatus status = NtStatus::SUCCESS;
        uint32_t bytes_written = 0;
    };

    struct DirEntry
    {
        uint64_t creation_time = 0;
        uint64_t last_access_time = 0;
        uint64_t last_write_time = 0;
        uint64_t change_time = 0;
        uint64_t end_of_file = 0;
        uint64_t allocation_size = 0;
        uint32_t file_attributes = 0;
        std::u16string file_name;
        FileId file_id;
    };

    struct LockResult
    {
        bool success = false;
        NtStatus status = NtStatus::SUCCESS;
    };

    struct IoctlResult
    {
        bool success = false;
        NtStatus status = NtStatus::SUCCESS;
        std::vector<uint8_t> output;
    };

    class SmbFileSystem
    {
    public:
        virtual ~SmbFileSystem() = default;

        virtual OpenResult open(const std::string &path, uint32_t desired_access,
                                uint32_t create_disposition, uint32_t create_options) = 0;
        virtual void close(void *handle) = 0;
        virtual ReadResult read(void *handle, uint64_t offset, uint32_t length) = 0;
        virtual WriteResult write(void *handle, uint64_t offset, const uint8_t *data, uint32_t length) = 0;
        virtual std::optional<std::vector<uint8_t> > query_info(void *handle, FileInfoClass info_class) = 0;
        virtual NtStatus set_info(void *handle, FileInfoClass info_class, const uint8_t *data, uint32_t len) = 0;
        virtual std::optional<std::vector<DirEntry> > query_directory(void *handle, const std::string &pattern,
                                                                      FileInfoClass info_class, bool restart) = 0;
        virtual NtStatus rename(void *handle, const std::string &new_path, bool replace) = 0;
        virtual NtStatus delete_file(void *handle) = 0;
        virtual NtStatus flush(void *handle) = 0;
        virtual LockResult lock(void *handle, uint64_t offset, uint64_t length, bool exclusive) = 0;
        virtual NtStatus unlock(void *handle, uint64_t offset, uint64_t length) = 0;
        virtual IoctlResult fsctl(void *handle, uint32_t code, const uint8_t *input, uint32_t input_len) = 0;
    };

    class LocalFileSystem : public SmbFileSystem
    {
    public:
        explicit LocalFileSystem(const std::string &root_path);
        ~LocalFileSystem() override;

        OpenResult open(const std::string &path, uint32_t desired_access,
                        uint32_t create_disposition, uint32_t create_options) override;
        void close(void *handle) override;
        ReadResult read(void *handle, uint64_t offset, uint32_t length) override;
        WriteResult write(void *handle, uint64_t offset, const uint8_t *data, uint32_t length) override;
        std::optional<std::vector<uint8_t> > query_info(void *handle, FileInfoClass info_class) override;
        NtStatus set_info(void *handle, FileInfoClass info_class, const uint8_t *data, uint32_t len) override;
        std::optional<std::vector<DirEntry> > query_directory(void *handle, const std::string &pattern,
                                                              FileInfoClass info_class, bool restart) override;
        NtStatus rename(void *handle, const std::string &new_path, bool replace) override;
        NtStatus delete_file(void *handle) override;
        NtStatus flush(void *handle) override;
        LockResult lock(void *handle, uint64_t offset, uint64_t length, bool exclusive) override;
        NtStatus unlock(void *handle, uint64_t offset, uint64_t length) override;
        IoctlResult fsctl(void *handle, uint32_t code, const uint8_t *input, uint32_t input_len) override;

    private:
        std::string root_path_;
        std::string resolve(const std::string &relative) const;
        static uint64_t filetime_now();
        static uint64_t filetime_from_timespec(const struct timespec &ts);
        uint32_t posix_to_file_attributes(mode_t mode) const;
    };
}
#endif
