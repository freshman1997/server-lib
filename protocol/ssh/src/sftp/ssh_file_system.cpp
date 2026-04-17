#include "sftp/ssh_file_system.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace yuan::net::ssh
{
    static std::string handle_id_to_string(uint64_t id)
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

    static SftpStatus errno_to_status(int err)
    {
        switch (err) {
        case ENOENT:
            return SftpStatus::SSH_FX_NO_SUCH_FILE;
        case EACCES:
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
        case ELOOP:
            return SftpStatus::SSH_FX_LINK_LOOP;
        case ENAMETOOLONG:
            return SftpStatus::SSH_FX_INVALID_FILENAME;
        default:
            return SftpStatus::SSH_FX_FAILURE;
        }
    }

    SshLocalFileSystem::SshLocalFileSystem(const std::string & root_dir)
        : root_dir_(root_dir)
    {
        if (!root_dir_.empty() && root_dir_.back() == '/') {
            root_dir_.pop_back();
        }
    }

    SshLocalFileSystem::~SshLocalFileSystem()
    {
        for (auto &kv : file_handles_) {
            ::close(kv.second);
        }
        file_handles_.clear();
        for (auto &kv : dir_handles_) {
            closedir(static_cast<DIR *>(kv.second));
        }
        dir_handles_.clear();
    }

    std::string SshLocalFileSystem::resolve_path(const std::string & path) const
    {
        if (path.empty() || path[0] != '/') {
            return "";
        }

        std::vector<std::string> parts;
        std::istringstream iss(path);
        std::string part;
        while (std::getline(iss, part, '/')) {
            if (part.empty() || part == ".")
                continue;
            if (part == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }

        if (parts.empty())
            return root_dir_.empty() ? "/" : root_dir_;

        std::string result = root_dir_.empty() ? "" : root_dir_;
        for (const auto &p : parts) {
            result += '/' + p;
        }

        char buf[PATH_MAX] = {};
        char *real = ::realpath(result.c_str(), buf);
        if (real) {
            std::string resolved = real;
            if (!root_dir_.empty() && resolved.size() >= root_dir_.size() &&
                resolved.compare(0, root_dir_.size(), root_dir_) == 0) {
                return resolved;
            }
            if (!root_dir_.empty()) {
                return "";
            }
            return resolved;
        }

        return result;
    }

    SftpFileAttrs SshLocalFileSystem::stat_to_attrs(const struct stat & st) const
    {
        SftpFileAttrs attrs;
        attrs.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID |
                      SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
        attrs.size = static_cast<uint64_t>(st.st_size);
        attrs.uid = static_cast<uint32_t>(st.st_uid);
        attrs.gid = static_cast<uint32_t>(st.st_gid);
        attrs.permissions = static_cast<uint32_t>(st.st_mode);
        attrs.atime = static_cast<uint32_t>(st.st_atime);
        attrs.mtime = static_cast<uint32_t>(st.st_mtime);
        return attrs;
    }

    std::string SshLocalFileSystem::build_longname(const std::string & filename, const struct stat & st) const
    {
        char mode_str[12] = {};
        mode_t m = st.st_mode;
        mode_str[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
        mode_str[1] = (m & S_IRUSR) ? 'r' : '-';
        mode_str[2] = (m & S_IWUSR) ? 'w' : '-';
        mode_str[3] = (m & S_IXUSR) ? 'x' : '-';
        mode_str[4] = (m & S_IRGRP) ? 'r' : '-';
        mode_str[5] = (m & S_IWGRP) ? 'w' : '-';
        mode_str[6] = (m & S_IXGRP) ? 'x' : '-';
        mode_str[7] = (m & S_IROTH) ? 'r' : '-';
        mode_str[8] = (m & S_IWOTH) ? 'w' : '-';
        mode_str[9] = (m & S_IXOTH) ? 'x' : '-';

        char time_buf[64] = {};
        struct tm tmbuf;
        localtime_r(&st.st_mtime, &tmbuf);
        strftime(time_buf, sizeof(time_buf), "%b %e %H:%M", &tmbuf);

        char longname[512] = {};
        snprintf(longname, sizeof(longname), "%s %3lu %5u %5u %10llu %s %s",
                 mode_str,
                 static_cast<unsigned long>(st.st_nlink),
                 static_cast<unsigned>(st.st_uid),
                 static_cast<unsigned>(st.st_gid),
                 static_cast<unsigned long long>(st.st_size),
                 time_buf,
                 filename.c_str());
        return std::string(longname);
    }

    SshFsOpenResult SshLocalFileSystem::open(const std::string & path, uint32_t pflags, const SftpFileAttrs &)
    {
        SshFsOpenResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        int flags = 0;
        if ((pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_READ)) &&
            (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_WRITE))) {
            flags |= O_RDWR;
        } else if (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_WRITE)) {
            flags |= O_WRONLY;
        } else {
            flags |= O_RDONLY;
        }

        if (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_CREAT)) {
            flags |= O_CREAT;
        }
        if (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_TRUNC)) {
            flags |= O_TRUNC;
        }
        if (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_EXCL)) {
            flags |= O_EXCL;
        }
        if (pflags & static_cast<uint32_t>(SftpOpenFlags::SSH_FXF_APPEND)) {
            flags |= O_APPEND;
        }

        int fd = ::open(resolved.c_str(), flags, 0644);
        if (fd < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        uint64_t hid = next_handle_id_++;
        std::string handle = handle_id_to_string(hid);
        file_handles_[handle] = fd;

        result.success = true;
        result.handle = std::move(handle);
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::close(const std::string & handle)
    {
        SshFsSimpleResult result;
        auto it = file_handles_.find(handle);
        if (it == file_handles_.end()) {
            auto dit = dir_handles_.find(handle);
            if (dit != dir_handles_.end()) {
                closedir(static_cast<DIR *>(dit->second));
                dir_handles_.erase(dit);
                result.success = true;
                result.status = SftpStatus::SSH_FX_OK;
                return result;
            }
            result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
            result.status_message = "Invalid handle";
            return result;
        }

        ::close(it->second);
        file_handles_.erase(it);
        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
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

        if (len > SFTP_MAX_READ_SIZE) {
            len = SFTP_MAX_READ_SIZE;
        }

        std::vector<uint8_t> buf(len);
        ssize_t n = ::pread(it->second, buf.data(), len, static_cast<off_t>(offset));
        if (n < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        if (n == 0) {
            result.eof = true;
            result.status = SftpStatus::SSH_FX_EOF;
            result.status_message = "End of file";
            return result;
        }

        buf.resize(static_cast<size_t>(n));
        result.success = true;
        result.data = std::move(buf);
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

        ssize_t n = ::pwrite(it->second, data, len, static_cast<off_t>(offset));
        if (n < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        if (static_cast<uint32_t>(n) != len) {
            result.status = SftpStatus::SSH_FX_FAILURE;
            result.status_message = "Short write";
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsStatResult SshLocalFileSystem::lstat(const std::string & path)
    {
        SshFsStatResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        struct stat st;
        if (::lstat(resolved.c_str(), &st) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.attrs = stat_to_attrs(st);
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

        struct stat st;
        if (::fstat(it->second, &st) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.attrs = stat_to_attrs(st);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::setstat(const std::string & path, const SftpFileAttrs & attrs)
    {
        SshFsSimpleResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_SIZE) {
            if (::truncate(resolved.c_str(), static_cast<off_t>(attrs.size)) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_UIDGID) {
            if (::chown(resolved.c_str(), static_cast<uid_t>(attrs.uid), static_cast<gid_t>(attrs.gid)) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            if (::chmod(resolved.c_str(), static_cast<mode_t>(attrs.permissions)) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
            struct timespec ts[2];
            ts[0].tv_sec = static_cast<time_t>(attrs.atime);
            ts[0].tv_nsec = 0;
            ts[1].tv_sec = static_cast<time_t>(attrs.mtime);
            ts[1].tv_nsec = 0;
            if (::utimensat(AT_FDCWD, resolved.c_str(), ts, 0) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::fsetstat(const std::string & handle, const SftpFileAttrs & attrs)
    {
        SshFsSimpleResult result;
        auto it = file_handles_.find(handle);
        if (it == file_handles_.end()) {
            result.status = SftpStatus::SSH_FX_INVALID_HANDLE;
            result.status_message = "Invalid handle";
            return result;
        }

        int fd = it->second;

        if (attrs.flags & SSH_FILEXFER_ATTR_SIZE) {
            if (::ftruncate(fd, static_cast<off_t>(attrs.size)) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_UIDGID) {
            if (::fchown(fd, static_cast<uid_t>(attrs.uid), static_cast<gid_t>(attrs.gid)) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            if (::fchmod(fd, static_cast<mode_t>(attrs.permissions)) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        if (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
            struct timespec ts[2];
            ts[0].tv_sec = static_cast<time_t>(attrs.atime);
            ts[0].tv_nsec = 0;
            ts[1].tv_sec = static_cast<time_t>(attrs.mtime);
            ts[1].tv_nsec = 0;
            if (::futimens(fd, ts) < 0) {
                result.status = errno_to_status(errno);
                result.status_message = std::strerror(errno);
                return result;
            }
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsOpenResult SshLocalFileSystem::opendir(const std::string & path)
    {
        SshFsOpenResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        DIR *dir = ::opendir(resolved.c_str());
        if (!dir) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        uint64_t hid = next_handle_id_++;
        std::string handle = handle_id_to_string(hid);
        dir_handles_[handle] = dir;

        result.success = true;
        result.handle = std::move(handle);
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

        DIR *dir = static_cast<DIR *>(it->second);
        std::vector<SftpNameEntry> entries;
        struct dirent *de = nullptr;

        for (int i = 0; i < 128; ++i) {
            errno = 0;
            de = ::readdir(dir);
            if (!de)
                break;

            if (std::strcmp(de->d_name, ".") == 0 || std::strcmp(de->d_name, "..") == 0)
                continue;

            SftpNameEntry entry;
            entry.filename = de->d_name;

            struct stat st;
            if (::fstatat(dirfd(dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                entry.attrs = stat_to_attrs(st);
                entry.longname = build_longname(de->d_name, st);
            } else {
                entry.attrs.flags = 0;
                entry.longname = de->d_name;
            }

            entries.push_back(std::move(entry));
        }

        if (entries.empty() && de == nullptr) {
            result.eof = true;
            result.status = SftpStatus::SSH_FX_EOF;
            result.status_message = "End of directory";
            return result;
        }

        result.success = true;
        result.entries = std::move(entries);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::remove(const std::string & path)
    {
        SshFsSimpleResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        if (::unlink(resolved.c_str()) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::mkdir(const std::string & path, const SftpFileAttrs & attrs)
    {
        SshFsSimpleResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        mode_t mode = 0755;
        if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
            mode = static_cast<mode_t>(attrs.permissions);
        }

        if (::mkdir(resolved.c_str(), mode) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::rmdir(const std::string & path)
    {
        SshFsSimpleResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        if (::rmdir(resolved.c_str()) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsRealPathResult SshLocalFileSystem::realpath(const std::string & path)
    {
        SshFsRealPathResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        char buf[PATH_MAX] = {};
        if (!::realpath(resolved.c_str(), buf)) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        std::string real = buf;
        if (real.size() >= root_dir_.size() && real.compare(0, root_dir_.size(), root_dir_) == 0) {
            real = real.substr(root_dir_.size());
            if (real.empty())
                real = "/";
        }

        struct stat st;
        if (::stat(resolved.c_str(), &st) == 0) {
            result.attrs = stat_to_attrs(st);
        }

        result.success = true;
        result.path = std::move(real);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsStatResult SshLocalFileSystem::stat(const std::string & path)
    {
        SshFsStatResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        struct stat st;
        if (::stat(resolved.c_str(), &st) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.attrs = stat_to_attrs(st);
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::rename(const std::string & old_path, const std::string & new_path, uint32_t flags)
    {
        SshFsSimpleResult result;
        auto resolved_old = resolve_path(old_path);
        auto resolved_new = resolve_path(new_path);
        if (resolved_old.empty() || resolved_new.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        struct stat new_st;
        bool new_exists = (::stat(resolved_new.c_str(), &new_st) == 0);

        if (new_exists) {
            if (!(flags & static_cast<uint32_t>(SftpRenameFlags::SSH_FXP_RENAME_OVERWRITE))) {
                result.status = SftpStatus::SSH_FX_FILE_ALREADY_EXISTS;
                result.status_message = "Target already exists";
                return result;
            }

            if (S_ISDIR(new_st.st_mode)) {
                result.status = SftpStatus::SSH_FX_FAILURE;
                result.status_message = "Cannot overwrite directory";
                return result;
            }
        }

        if (::rename(resolved_old.c_str(), resolved_new.c_str()) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsReadLinkResult SshLocalFileSystem::readlink(const std::string & path)
    {
        SshFsReadLinkResult result;
        auto resolved = resolve_path(path);
        if (resolved.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid path";
            return result;
        }

        char buf[PATH_MAX] = {};
        ssize_t n = ::readlink(resolved.c_str(), buf, sizeof(buf) - 1);
        if (n < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        buf[n] = '\0';

        struct stat st;
        if (::lstat(resolved.c_str(), &st) == 0) {
            result.attrs = stat_to_attrs(st);
        }

        result.success = true;
        result.link_target = std::string(buf, static_cast<size_t>(n));
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }

    SshFsSimpleResult SshLocalFileSystem::symlink(const std::string & link_path, const std::string & target_path)
    {
        SshFsSimpleResult result;
        auto resolved_link = resolve_path(link_path);
        if (resolved_link.empty()) {
            result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
            result.status_message = "Invalid link path";
            return result;
        }

        std::string resolved_target = target_path;
        if (!target_path.empty() && target_path[0] == '/') {
            resolved_target = resolve_path(target_path);
            if (resolved_target.empty()) {
                result.status = SftpStatus::SSH_FX_NO_SUCH_PATH;
                result.status_message = "Invalid target path";
                return result;
            }
        }

        if (::symlink(resolved_target.c_str(), resolved_link.c_str()) < 0) {
            result.status = errno_to_status(errno);
            result.status_message = std::strerror(errno);
            return result;
        }

        result.success = true;
        result.status = SftpStatus::SSH_FX_OK;
        return result;
    }
}
