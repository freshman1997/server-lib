#include "sftp/ssh_file_system.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace yuan::net::ssh
{
    namespace
    {
        using FsPath = std::filesystem::path;

        std::string handle_id_to_string(uint64_t id)
        {
            std::string s(8, '\0');
            s[0] = static_cast<char>((id >> 56) & 0xFF);
            s[1] = static_cast<char>((id >> 48) & 0xFF);
            s[2] = static_cast<char>((id >> 40) & 0xFF);
            s[3] = static_cast<char>((id >> 32) & 0xFF);
            s[4] = static_cast<char>((id >> 24) & 0xFF);
            s[5] = static_cast<char>((id >> 16) & 0xFF);
            s[6] = static_cast<char>((id >> 8) & 0xFF);
            s[7] = static_cast<char>(id & 0xFF);
            return s;
        }

        SftpStatus errno_to_status(int err)
        {
            switch (err) {
            case ENOENT:
                return SftpStatus::SSH_FX_NO_SUCH_FILE;
            case EACCES:
            case EPERM:
                return SftpStatus::SSH_FX_PERMISSION_DENIED;
            case EEXIST:
                return SftpStatus::SSH_FX_FILE_ALREADY_EXISTS;
            case ENOTDIR:
                return SftpStatus::SSH_FX_NOT_A_DIRECTORY;
            case ENOTEMPTY:
                return SftpStatus::SSH_FX_DIR_NOT_EMPTY;
            case EINVAL:
                return SftpStatus::SSH_FX_BAD_MESSAGE;
            case ENOSPC:
                return SftpStatus::SSH_FX_NO_SPACE_ON_FILESYSTEM;
            case EROFS:
                return SftpStatus::SSH_FX_WRITE_PROTECT;
#ifdef ELOOP
            case ELOOP:
                return SftpStatus::SSH_FX_LINK_LOOP;
#endif
            case ENAMETOOLONG:
                return SftpStatus::SSH_FX_INVALID_FILENAME;
            default:
                return SftpStatus::SSH_FX_FAILURE;
            }
        }

        SftpStatus error_code_to_status(const std::error_code & ec)
        {
            if (!ec) {
                return SftpStatus::SSH_FX_OK;
            }
            if (ec == std::make_error_code(std::errc::function_not_supported) ||
                ec == std::make_error_code(std::errc::operation_not_supported) ||
                ec == std::make_error_code(std::errc::not_supported)) {
                return SftpStatus::SSH_FX_OP_UNSUPPORTED;
            }
            return errno_to_status(ec.value());
        }

        std::string status_message_from_error(const std::error_code & ec, const char * fallback)
        {
            if (ec) {
                return ec.message();
            }
            return fallback;
        }

        bool seek_file(std::FILE * file, uint64_t offset)
        {
            if (!file) {
                return false;
            }
#ifdef _WIN32
            return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
            return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        uint64_t file_size_from_path(const FsPath & path)
        {
            std::error_code ec;
            auto size = std::filesystem::file_size(path, ec);
            return ec ? 0 : static_cast<uint64_t>(size);
        }

        uint32_t perms_to_sftp(std::filesystem::perms perms, bool is_dir, bool is_symlink)
        {
            uint32_t mode = 0;
            mode |= is_dir ? 0040000u : is_symlink ? 0120000u : 0100000u;

            if ((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) mode |= 0400u;
            if ((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none) mode |= 0200u;
            if ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) mode |= 0100u;
            if ((perms & std::filesystem::perms::group_read) != std::filesystem::perms::none) mode |= 0040u;
            if ((perms & std::filesystem::perms::group_write) != std::filesystem::perms::none) mode |= 0020u;
            if ((perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none) mode |= 0010u;
            if ((perms & std::filesystem::perms::others_read) != std::filesystem::perms::none) mode |= 0004u;
            if ((perms & std::filesystem::perms::others_write) != std::filesystem::perms::none) mode |= 0002u;
            if ((perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none) mode |= 0001u;
            return mode;
        }

        std::string perms_to_longname(uint32_t permissions)
        {
            std::array<char, 11> mode_str = { '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '\0' };
            const uint32_t file_type = permissions & 0170000u;
            mode_str[0] = (file_type == 0040000u) ? 'd' : (file_type == 0120000u) ? 'l' : '-';
            mode_str[1] = (permissions & 0400u) ? 'r' : '-';
            mode_str[2] = (permissions & 0200u) ? 'w' : '-';
            mode_str[3] = (permissions & 0100u) ? 'x' : '-';
            mode_str[4] = (permissions & 0040u) ? 'r' : '-';
            mode_str[5] = (permissions & 0020u) ? 'w' : '-';
            mode_str[6] = (permissions & 0010u) ? 'x' : '-';
            mode_str[7] = (permissions & 0004u) ? 'r' : '-';
            mode_str[8] = (permissions & 0002u) ? 'w' : '-';
            mode_str[9] = (permissions & 0001u) ? 'x' : '-';
            return std::string(mode_str.data());
        }

        uint32_t file_time_to_unix_seconds(std::filesystem::file_time_type ft)
        {
            using namespace std::chrono;
            const auto system_now = system_clock::now();
            const auto file_now = std::filesystem::file_time_type::clock::now();
            const auto system_tp = time_point_cast<system_clock::duration>(ft - file_now + system_now);
            const auto seconds = duration_cast<std::chrono::seconds>(system_tp.time_since_epoch()).count();
            return seconds < 0 ? 0u : static_cast<uint32_t>(seconds);
        }

        std::filesystem::file_time_type unix_seconds_to_file_time(uint32_t value)
        {
            using namespace std::chrono;
            const auto system_tp = system_clock::time_point{} + seconds(value);
            const auto system_now = system_clock::now();
            const auto file_now = std::filesystem::file_time_type::clock::now();
            return time_point_cast<std::filesystem::file_time_type::duration>(system_tp - system_now + file_now);
        }

        std::string to_generic_string(const FsPath & path)
        {
            return path.generic_string();
        }
    }

    SshLocalFileSystem::SshLocalFileSystem(const std::string & root_dir)
        : root_dir_(root_dir)
    {
        if (root_dir_.empty()) {
#ifdef _WIN32
            root_dir_ = std::filesystem::current_path().generic_string();
#else
            root_dir_ = "/";
#endif
        }
    }

    SshLocalFileSystem::~SshLocalFileSystem()
    {
        for (auto & kv : file_handles_) {
            if (kv.second.file) {
                std::fclose(kv.second.file);
            }
        }
        file_handles_.clear();
        dir_handles_.clear();
    }

    std::filesystem::path SshLocalFileSystem::normalized_root_path() const
    {
        std::error_code ec;
        auto root = std::filesystem::absolute(FsPath(root_dir_), ec);
        if (ec) {
            root = FsPath(root_dir_);
        }
        root = root.lexically_normal();
        return root;
    }

    std::string SshLocalFileSystem::resolve_path(const std::string & path) const
    {
        if (path.empty() || path[0] != '/') {
            return "";
        }

        for (const auto & part : FsPath(path)) {
            if (part == "..") {
                return "";
            }
        }

        const auto root = normalized_root_path();
        FsPath relative;
        for (const auto & part : FsPath(path).lexically_normal()) {
            if (part == "/" || part == ".") {
                continue;
            }
            if (part == "..") {
                return "";
            }
            relative /= part;
        }

        const auto candidate = (root / relative).lexically_normal();

        auto root_it = root.begin();
        auto cand_it = candidate.begin();
        for (; root_it != root.end() && cand_it != candidate.end(); ++root_it, ++cand_it) {
            if (*root_it != *cand_it) {
                return "";
            }
        }
        if (root_it != root.end()) {
            return "";
        }

        auto preferred = candidate;
        preferred.make_preferred();
        return preferred.string();
    }

    SftpFileAttrs SshLocalFileSystem::stat_to_attrs(const FsPath & path,
                                                    const std::filesystem::file_status & status,
                                                    bool follow_symlinks) const
    {
        SftpFileAttrs attrs;
        attrs.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;

        const bool is_dir = std::filesystem::is_directory(status);
        const bool is_symlink = std::filesystem::is_symlink(status);
        if (!is_dir && follow_symlinks) {
            attrs.size = file_size_from_path(path);
        } else if (!is_dir && !is_symlink) {
            attrs.size = file_size_from_path(path);
        }

        attrs.permissions = perms_to_sftp(status.permissions(), is_dir, is_symlink);

        std::error_code ec;
        attrs.mtime = file_time_to_unix_seconds(std::filesystem::last_write_time(path, ec));
        attrs.atime = attrs.mtime;
        return attrs;
    }

    std::string SshLocalFileSystem::build_longname(const std::string & filename,
                                                   const FsPath & path,
                                                   const std::filesystem::file_status & status) const
    {
        const auto attrs = stat_to_attrs(path, status, true);
        const auto timestamp = static_cast<std::time_t>(attrs.mtime);
        char time_buf[64] = {};
        std::tm tm_buf = {};
#ifdef _WIN32
        localtime_s(&tm_buf, &timestamp);
#else
        localtime_r(&timestamp, &tm_buf);
#endif
        std::strftime(time_buf, sizeof(time_buf), "%b %e %H:%M", &tm_buf);

        std::ostringstream oss;
        oss << perms_to_longname(attrs.permissions)
            << "   1 "
            << "     0 "
            << "     0 "
            << std::setw(10) << attrs.size
            << ' ' << time_buf
            << ' ' << filename;
        return oss.str();
    }

    SshFsOpenResult SshLocalFileSystem::open(const std::string & path, uint32_t pflags, const SftpFileAttrs &)
    {
        SshFsOpenResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        const FsPath fs_path(resolved);
        const bool want_read = (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_READ)) != 0;
        const bool want_write = (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_WRITE)) != 0;
        const bool want_append = (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_APPEND)) != 0;
        const bool want_create = (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_CREAT)) != 0;
        const bool want_trunc = (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_TRUNC)) != 0;
        const bool want_excl = (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_EXCL)) != 0;

        std::error_code ec;
        const bool exists = std::filesystem::exists(fs_path, ec);
        if (ec) {
            result.status = error_code_to_status(ec);
            result.status_message = ec.message();
            return result;
        }

        if (want_excl && want_create && exists) {
            result.status = SftpStatus::SSH_FX_FILE_ALREADY_EXISTS;
            result.status_message = "Target already exists";
            return result;
        }

        if (!exists && !want_create) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_FILE;
            result.status_message = "No such file";
            return result;
        }

        if (!exists && want_create) {
            std::FILE * create_file = std::fopen(fs_path.string().c_str(), "w+b");
            if (!create_file) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
            std::fclose(create_file);
        }

        const char * mode = (want_read || !want_write) ? "r+b" : "r+b";
        if (!std::filesystem::exists(fs_path, ec)) {
            mode = "w+b";
        }

        std::FILE * file = std::fopen(fs_path.string().c_str(), mode);
        if (!file && want_create) {
            file = std::fopen(fs_path.string().c_str(), "w+b");
        }
        if (!file && !want_write && want_read) {
            file = std::fopen(fs_path.string().c_str(), "rb");
        }
        if (!file) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        if (want_trunc) {
            std::error_code truncate_ec;
            std::filesystem::resize_file(fs_path, 0, truncate_ec);
            if (truncate_ec) {
                std::fclose(file);
                result.status = error_code_to_status(truncate_ec);
                result.status_message = truncate_ec.message();
                return result;
            }
        }

        const auto handle = handle_id_to_string(next_handle_id_++);
        file_handles_[handle] = FileHandleState{ file, fs_path, want_append };
        result.success = true;
        result.handle = handle;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::close(const std::string & handle)
    {
        SshFsSimpleResult result;
        auto fit = file_handles_.find(handle);
        if (fit != file_handles_.end()) {
            std::fclose(fit->second.file);
            file_handles_.erase(fit);
            result.success = true;
            result.status = SftpStatus::SSH_FX_OK;
            return result;
        }

        auto dit = dir_handles_.find(handle);
        if (dit != dir_handles_.end()) {
            dir_handles_.erase(dit);
            result.success = true;
            result.status = SftpStatus::SSH_FX_OK;
            return result;
        }

        result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
        result.status_message = "Invalid handle";
        return result;
    }

    SshFsReadResult SshLocalFileSystem::read(const std::string & handle, uint64_t offset, uint32_t len)
    {
        SshFsReadResult result;
        auto it = file_handles_.find(handle);
        if (it == file_handles_.end()) {
            result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
            result.status_message = "Invalid handle";
            return result;
        }

        len = std::min<uint32_t>(len, SFTP_MAX_READ_SIZE);
        if (!seek_file(it->second.file, offset)) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        std::vector<uint8_t> buffer(len);
        const size_t count = std::fread(buffer.data(), 1, len, it->second.file);
        if (count == 0 && std::ferror(it->second.file)) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        if (count == 0) {
            result.eof = true;
            result.status = SftpStatus::SSH_FX_EOF;
            result.status_message = "End of file";
            return result;
        }

        buffer.resize(count);
        result.success = true;
        result.data = std::move(buffer);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsWriteResult SshLocalFileSystem::write(const std::string & handle, uint64_t offset, const uint8_t * data, uint32_t len)
    {
        SshFsWriteResult result;
        auto it = file_handles_.find(handle);
        if (it == file_handles_.end()) {
            result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
            result.status_message = "Invalid handle";
            return result;
        }

        auto & state = it->second;
        uint64_t write_offset = offset;
        if (state.append) {
            write_offset = file_size_from_path(state.path);
        }

        if (!seek_file(state.file, write_offset)) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        const size_t written = std::fwrite(data, 1, len, state.file);
        std::fflush(state.file);
        if (written != len) {
            result.status = errno_to_status(errno);
            result.status_message = written == 0 ? std::strerror(errno) : "Short write";
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsStatResult SshLocalFileSystem::lstat(const std::string & path)
    {
        SshFsStatResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        const auto status = std::filesystem::symlink_status(resolved, ec);
        if (ec || status.type() == std::filesystem::file_type::not_found) {
            result.status = error_code_to_status(ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
            result.status_message = status_message_from_error(ec, "No such path");
            return result;
        }

        result.success = true;
        result.attrs = stat_to_attrs(resolved, status, false);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsStatResult SshLocalFileSystem::fstat(const std::string & handle)
    {
        SshFsStatResult result;
        auto it = file_handles_.find(handle);
        if (it == file_handles_.end()) {
            result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
            result.status_message = "Invalid handle";
            return result;
        }

        std::error_code ec;
        const auto status = std::filesystem::status(it->second.path, ec);
        if (ec) {
            result.status = error_code_to_status(ec);
            result.status_message = ec.message();
            return result;
        }

        result.success = true;
        result.attrs = stat_to_attrs(it->second.path, status, true);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::setstat(const std::string & path, const SftpFileAttrs & attrs)
    {
        SshFsSimpleResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        const FsPath fs_path(resolved);
        std::error_code ec;

        if (attrs.flags & SSH_FILEXFER_ATTR_SIZE) {
            std::filesystem::resize_file(fs_path, attrs.size, ec);
            if (ec) {
                result.status = error_code_to_status(ec);
                result.status_message = ec.message();
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            const auto perms = static_cast<std::filesystem::perms>(attrs.permissions & 0777u);
            std::filesystem::permissions(fs_path, perms, std::filesystem::perm_options::replace, ec);
            if (ec) {
                result.status = error_code_to_status(ec);
                result.status_message = ec.message();
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
            std::filesystem::last_write_time(fs_path, unix_seconds_to_file_time(attrs.mtime), ec);
            if (ec) {
                result.status = error_code_to_status(ec);
                result.status_message = ec.message();
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_UIDGID) {
            result.status = SftpStatus::SSH_FX_OP_UNSUPPORTED;
            result.status_message = "UID/GID updates are not supported by the local SFTP filesystem";
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::fsetstat(const std::string & handle, const SftpFileAttrs & attrs)
    {
        auto it = file_handles_.find(handle);
        if (it == file_handles_.end()) {
            SshFsSimpleResult result;
            result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
            result.status_message = "Invalid handle";
            return result;
        }

        std::string logical_path = "/";
        const auto root = normalized_root_path();
        std::error_code ec;
        auto relative = std::filesystem::relative(it->second.path, root, ec);
        if (!ec) {
            logical_path += relative.generic_string();
        }
        return setstat(logical_path, attrs);
    }

    SshFsOpenResult SshLocalFileSystem::opendir(const std::string & path)
    {
        SshFsOpenResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(resolved, ec) || ec) {
            result.status = ec ? error_code_to_status(ec) : SftpStatus::SSH_FX_NOT_A_DIRECTORY;
            result.status_message = ec ? ec.message() : "Not a directory";
            return result;
        }

        DirHandleState state;
        for (const auto & entry : std::filesystem::directory_iterator(resolved, ec)) {
            if (ec) {
                result.status = error_code_to_status(ec);
                result.status_message = ec.message();
                return result;
            }

            const auto filename = entry.path().filename().string();
            if (filename == "." || filename == "..") {
                continue;
            }

            SftpNameEntry name_entry;
            name_entry.filename = filename;
            const auto status = entry.symlink_status(ec);
            if (!ec) {
                name_entry.attrs = stat_to_attrs(entry.path(), status, false);
                name_entry.longname = build_longname(filename, entry.path(), status);
            } else {
                name_entry.longname = filename;
                ec.clear();
            }
            state.entries.push_back(std::move(name_entry));
        }

        const auto handle = handle_id_to_string(next_handle_id_++);
        dir_handles_[handle] = std::move(state);
        result.success = true;
        result.handle = handle;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsReadDirResult SshLocalFileSystem::readdir(const std::string & handle)
    {
        SshFsReadDirResult result;
        auto it = dir_handles_.find(handle);
        if (it == dir_handles_.end()) {
            result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
            result.status_message = "Invalid handle";
            return result;
        }

        auto & state = it->second;
        if (state.cursor >= state.entries.size()) {
            result.eof = true;
            result.status = SftpStatus::SSH_FX_EOF;
            result.status_message = "End of directory";
            return result;
        }

        const size_t batch_end = std::min(state.cursor + 128u, state.entries.size());
        result.entries.assign(state.entries.begin() + static_cast<std::ptrdiff_t>(state.cursor),
                              state.entries.begin() + static_cast<std::ptrdiff_t>(batch_end));
        state.cursor = batch_end;
        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::remove(const std::string & path)
    {
        SshFsSimpleResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        if (!std::filesystem::remove(resolved, ec)) {
            result.status = ec ? error_code_to_status(ec) : SftpStatus::SSH_FX_NO_SUCH_FILE;
            result.status_message = ec ? ec.message() : "No such file";
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::mkdir(const std::string & path, const SftpFileAttrs & attrs)
    {
        SshFsSimpleResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        if (!std::filesystem::create_directory(resolved, ec)) {
            result.status = ec ? error_code_to_status(ec) : SftpStatus::SSH_FX_FILE_ALREADY_EXISTS;
            result.status_message = ec ? ec.message() : "Directory already exists";
            return result;
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            std::filesystem::permissions(resolved,
                                         static_cast<std::filesystem::perms>(attrs.permissions & 0777u),
                                         std::filesystem::perm_options::replace,
                                         ec);
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::rmdir(const std::string & path)
    {
        SshFsSimpleResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        if (!std::filesystem::remove(resolved, ec)) {
            result.status = ec ? error_code_to_status(ec) : SftpStatus::SSH_FX_DIR_NOT_EMPTY;
            result.status_message = ec ? ec.message() : "Directory not empty";
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsRealPathResult SshLocalFileSystem::realpath(const std::string & path)
    {
        SshFsRealPathResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(resolved, ec);
        if (ec) {
            result.status = error_code_to_status(ec);
            result.status_message = ec.message();
            return result;
        }

        const auto root = normalized_root_path();
        auto relative = std::filesystem::relative(canonical, root, ec);
        const bool is_root_relative = !ec && (relative.empty() || relative == FsPath("."));
        result.path = ec ? "/" : (is_root_relative ? "/" : "/" + relative.generic_string());

        const auto status = std::filesystem::status(canonical, ec);
        if (!ec) {
            result.attrs = stat_to_attrs(canonical, status, true);
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsStatResult SshLocalFileSystem::stat(const std::string & path)
    {
        SshFsStatResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        const auto status = std::filesystem::status(resolved, ec);
        if (ec || status.type() == std::filesystem::file_type::not_found) {
            result.status = error_code_to_status(ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
            result.status_message = status_message_from_error(ec, "No such path");
            return result;
        }

        result.success = true;
        result.attrs = stat_to_attrs(resolved, status, true);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::rename(const std::string & old_path, const std::string & new_path, uint32_t flags)
    {
        SshFsSimpleResult result;
        const auto resolved_old = resolve_path(old_path);
        const auto resolved_new = resolve_path(new_path);
        if (resolved_old.empty() || resolved_new.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        const bool new_exists = std::filesystem::exists(resolved_new, ec);
        if (ec) {
            result.status = error_code_to_status(ec);
            result.status_message = ec.message();
            return result;
        }

        if (new_exists) {
            if (!(flags & static_cast<uint32_t>(SftpRenameFlags::SSH_FXP_RENAME_OVERWRITE))) {
                result.status = SftpStatus::SSH_FX_FILE_ALREADY_EXISTS;
                result.status_message = "Target already exists";
                return result;
            }

            if (std::filesystem::is_directory(resolved_new, ec)) {
                result.status = SftpStatus::SSH_FX_FAILURE;
                result.status_message = "Cannot overwrite directory";
                return result;
            }

            std::filesystem::remove(resolved_new, ec);
            if (ec) {
                result.status = error_code_to_status(ec);
                result.status_message = ec.message();
                return result;
            }
        }

        std::filesystem::rename(resolved_old, resolved_new, ec);
        if (ec) {
            result.status = error_code_to_status(ec);
            result.status_message = ec.message();
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsReadLinkResult SshLocalFileSystem::readlink(const std::string & path)
    {
        SshFsReadLinkResult result;
        const auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        std::error_code ec;
        const auto target = std::filesystem::read_symlink(resolved, ec);
        if (ec) {
            result.status = error_code_to_status(ec);
            result.status_message = ec.message();
            return result;
        }

        const auto status = std::filesystem::symlink_status(resolved, ec);
        if (!ec) {
            result.attrs = stat_to_attrs(resolved, status, false);
        }

        std::string exposed_target = to_generic_string(target);
        const auto root = normalized_root_path();
        const auto target_candidate = target.is_absolute()
                                          ? target.lexically_normal()
                                          : (FsPath(resolved).parent_path() / target).lexically_normal();
        auto root_it = root.begin();
        auto candidate_it = target_candidate.begin();
        for (; root_it != root.end() && candidate_it != target_candidate.end(); ++root_it, ++candidate_it) {
            if (*root_it != *candidate_it) {
                candidate_it = target_candidate.end();
                break;
            }
        }
        if (root_it == root.end() && candidate_it != target_candidate.end()) {
            std::error_code relative_ec;
            const auto logical = std::filesystem::relative(target_candidate, root, relative_ec);
            if (!relative_ec) {
                exposed_target = logical.empty() ? "/" : "/" + logical.generic_string();
            }
        }

        result.success = true;
        result.link_target = std::move(exposed_target);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::symlink(const std::string & link_path, const std::string & target_path)
    {
        SshFsSimpleResult result;
        const auto resolved_link = resolve_path(link_path);
        if (resolved_link.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid link path";
            return result;
        }

        FsPath link_target = target_path;
        if (!target_path.empty() && target_path[0] == '/') {
            const auto resolved_target = resolve_path(target_path);
            if (resolved_target.empty()) {
                result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
                result.status_message = "Invalid target path";
                return result;
            }
            std::error_code relative_ec;
            link_target = std::filesystem::relative(
                FsPath(resolved_target),
                FsPath(resolved_link).parent_path(),
                relative_ec);
            if (relative_ec) {
                result.status = error_code_to_status(relative_ec);
                result.status_message = relative_ec.message();
                return result;
            }
        }

        std::error_code ec;
        const bool target_is_directory = std::filesystem::is_directory(link_target, ec);
        ec.clear();
#ifdef _WIN32
        if (target_is_directory) {
            std::filesystem::create_directory_symlink(link_target, resolved_link, ec);
        } else {
            std::filesystem::create_symlink(link_target, resolved_link, ec);
        }
#else
        std::filesystem::create_symlink(link_target, resolved_link, ec);
#endif
        if (ec) {
            result.status = error_code_to_status(ec);
            result.status_message = ec.message();
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }
}
