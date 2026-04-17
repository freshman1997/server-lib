#include "smb_file_system.h"
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace yuan::net::smb
{
    static constexpr uint64_t EPOCH_DIFFERENCE = 116444736000000000ULL;

    LocalFileSystem::LocalFileSystem(const std::string & root_path)
        : root_path_(root_path)
    {
        if (!root_path_.empty() && root_path_.back() == '/') {
            root_path_.pop_back();
        }
    }

    LocalFileSystem::~LocalFileSystem() = default;

    std::string LocalFileSystem::resolve(const std::string & relative) const
    {
        std::string normalized = relative;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        while (!normalized.empty() && normalized[0] == '/') {
            normalized.erase(normalized.begin());
        }

        namespace fs = std::filesystem;
        fs::path root(root_path_);
        fs::path combined = root / normalized;
        fs::path canonical = fs::weakly_canonical(combined);
        fs::path canonical_root = fs::weakly_canonical(root);

        auto root_str = canonical_root.string();
        auto canon_str = canonical.string();

        if (canon_str.size() < root_str.size() ||
            canon_str.substr(0, root_str.size()) != root_str) {
            return root_path_;
        }

        return canonical.string();
    }

    uint64_t LocalFileSystem::filetime_now()
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return filetime_from_timespec(ts);
    }

    uint64_t LocalFileSystem::filetime_from_timespec(const struct timespec & ts)
    {
        uint64_t result = EPOCH_DIFFERENCE;
        result += static_cast<uint64_t>(ts.tv_sec) * 10000000ULL;
        result += static_cast<uint64_t>(ts.tv_nsec) / 100ULL;
        return result;
    }

    uint32_t LocalFileSystem::posix_to_file_attributes(mode_t mode) const
    {
        uint32_t attrs = 0;
        if (S_ISDIR(mode)) {
            attrs |= FILE_ATTRIBUTE_DIRECTORY;
        } else {
            attrs |= FILE_ATTRIBUTE_NORMAL;
        }
        if (!(mode & S_IWUSR)) {
            attrs |= FILE_ATTRIBUTE_READONLY;
        }
        if (S_ISLNK(mode)) {
            attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
        }
        return attrs;
    }

    OpenResult LocalFileSystem::open(const std::string & path, uint32_t desired_access,
                                     uint32_t create_disposition, uint32_t create_options)
    {
        OpenResult result;
        std::string full_path = resolve(path);

        int flags = 0;
        if (desired_access & GENERIC_WRITE || desired_access & FILE_WRITE_DATA) {
            if (desired_access & GENERIC_READ || desired_access & FILE_READ_DATA) {
                flags |= O_RDWR;
            } else {
                flags |= O_WRONLY;
            }
        } else {
            flags |= O_RDONLY;
        }

        switch (create_disposition) {
        case FILE_SUPERSEDE:
        case FILE_OVERWRITE:
            flags |= O_CREAT | O_TRUNC;
            break;
        case FILE_OVERWRITE_IF:
            flags |= O_CREAT | O_TRUNC;
            break;
        case FILE_CREATE:
            flags |= O_CREAT | O_EXCL;
            break;
        case FILE_OPEN:
            break;
        case FILE_OPEN_IF:
            flags |= O_CREAT;
            break;
        default:
            break;
        }

        if (create_options & FILE_DIRECTORY_FILE) {
            flags |= O_DIRECTORY;
        }

        int fd = ::open(full_path.c_str(), flags, 0644);
        if (fd < 0) {
            result.success = false;
            switch (errno) {
            case ENOENT:
                result.status = NtStatus::OBJECT_NAME_NOT_FOUND;
                break;
            case EEXIST:
                result.status = NtStatus::OBJECT_NAME_COLLISION;
                break;
            case EACCES:
                result.status = NtStatus::ACCESS_DENIED;
                break;
            default:
                result.status = NtStatus::UNSUCCESSFUL;
                break;
            }
            return result;
        }

        struct stat st;
        if (fstat(fd, &st) != 0) {
            ::close(fd);
            result.success = false;
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }

        result.success = true;
        result.status = NtStatus::SUCCESS;
        result.handle = reinterpret_cast<void *>(static_cast<intptr_t>(fd));
        result.is_directory = S_ISDIR(st.st_mode);
        result.allocation_size = static_cast<uint64_t>(st.st_blocks) * 512;
        result.end_of_file = static_cast<uint64_t>(st.st_size);
        result.file_attributes = posix_to_file_attributes(st.st_mode);

        struct timespec ts;
#ifdef STAT_TIMESPEC
        ts = st.STAT_TIMESPEC(st_atim);
        result.last_access_time = filetime_from_timespec(ts);
        ts = st.STAT_TIMESPEC(st_mtim);
        result.last_write_time = filetime_from_timespec(ts);
        ts = st.STAT_TIMESPEC(st_ctim);
        result.change_time = filetime_from_timespec(ts);
#else
        result.last_access_time = filetime_from_timespec(st.st_atim);
        result.last_write_time = filetime_from_timespec(st.st_mtim);
        result.change_time = filetime_from_timespec(st.st_ctim);
#endif
        result.creation_time = result.change_time;

        if (create_disposition == FILE_CREATE || create_disposition == FILE_OPEN_IF) {
            if (result.end_of_file == 0 && !result.is_directory) {
                result.create_action = FILE_CREATED;
            } else {
                result.create_action = FILE_OPENED;
            }
        } else if (create_disposition == FILE_OVERWRITE || create_disposition == FILE_OVERWRITE_IF || create_disposition == FILE_SUPERSEDE) {
            result.create_action = FILE_OVERWRITTEN;
        } else {
            result.create_action = FILE_OPENED;
        }

        return result;
    }

    void LocalFileSystem::close(void * handle)
    {
        if (handle) {
            int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
            ::close(fd);
        }
    }

    ReadResult LocalFileSystem::read(void * handle, uint64_t offset, uint32_t length)
    {
        ReadResult result;
        if (!handle) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        result.data.resize(length);

        ssize_t n = pread64(fd, result.data.data(), length, static_cast<off64_t>(offset));
        if (n < 0) {
            result.success = false;
            result.status = NtStatus::UNSUCCESSFUL;
            result.data.clear();
            return result;
        }

        result.data.resize(static_cast<size_t>(n));
        result.success = true;
        result.status = NtStatus::SUCCESS;
        result.bytes_read = static_cast<uint32_t>(n);
        return result;
    }

    WriteResult LocalFileSystem::write(void * handle, uint64_t offset, const uint8_t * data, uint32_t length)
    {
        WriteResult result;
        if (!handle) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        ssize_t n = pwrite64(fd, data, length, static_cast<off64_t>(offset));
        if (n < 0) {
            result.success = false;
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }

        result.success = true;
        result.status = NtStatus::SUCCESS;
        result.bytes_written = static_cast<uint32_t>(n);
        return result;
    }

    std::optional<std::vector<uint8_t> > LocalFileSystem::query_info(void * handle, FileInfoClass info_class)
    {
        if (!handle)
            return std::nullopt;

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        struct stat st;
        if (fstat(fd, &st) != 0)
            return std::nullopt;

        std::vector<uint8_t> buf;

        switch (info_class) {
        case FileInfoClass::FileBasicInformation: {
            buf.resize(40);
            uint64_t ct = filetime_from_timespec(st.st_ctim);
            uint64_t at = filetime_from_timespec(st.st_atim);
            uint64_t wt = filetime_from_timespec(st.st_mtim);
            uint64_t cht = ct;
            uint32_t attrs = posix_to_file_attributes(st.st_mode);
            std::memcpy(buf.data() + 0, &ct, 8);
            std::memcpy(buf.data() + 8, &at, 8);
            std::memcpy(buf.data() + 16, &wt, 8);
            std::memcpy(buf.data() + 24, &cht, 8);
            std::memcpy(buf.data() + 32, &attrs, 4);
            std::memset(buf.data() + 36, 0, 4);
            break;
        }
        case FileInfoClass::FileStandardInformation: {
            buf.resize(24);
            uint64_t alloc = static_cast<uint64_t>(st.st_blocks) * 512;
            uint64_t eof = static_cast<uint64_t>(st.st_size);
            uint32_t nl = static_cast<uint32_t>(st.st_nlink);
            uint8_t del_pending = 0;
            uint8_t is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            std::memcpy(buf.data() + 0, &alloc, 8);
            std::memcpy(buf.data() + 8, &eof, 8);
            std::memcpy(buf.data() + 16, &nl, 4);
            std::memcpy(buf.data() + 20, &del_pending, 1);
            std::memcpy(buf.data() + 21, &is_dir, 1);
            std::memset(buf.data() + 22, 0, 2);
            break;
        }
        case FileInfoClass::FileInternalInformation: {
            buf.resize(8);
            uint64_t ino = static_cast<uint64_t>(st.st_ino);
            std::memcpy(buf.data(), &ino, 8);
            break;
        }
        case FileInfoClass::FileAllInformation: {
            buf.resize(70);
            uint64_t ct = filetime_from_timespec(st.st_ctim);
            uint64_t at = filetime_from_timespec(st.st_atim);
            uint64_t wt = filetime_from_timespec(st.st_mtim);
            uint32_t attrs = posix_to_file_attributes(st.st_mode);
            uint64_t alloc = static_cast<uint64_t>(st.st_blocks) * 512;
            uint64_t eof = static_cast<uint64_t>(st.st_size);
            uint32_t nl = static_cast<uint32_t>(st.st_nlink);
            uint8_t del_pending = 0;
            uint8_t is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            uint64_t ino = static_cast<uint64_t>(st.st_ino);
            std::memcpy(buf.data() + 0, &ct, 8);
            std::memcpy(buf.data() + 8, &at, 8);
            std::memcpy(buf.data() + 16, &wt, 8);
            std::memcpy(buf.data() + 24, &ct, 8);
            std::memcpy(buf.data() + 32, &attrs, 4);
            std::memset(buf.data() + 36, 0, 4);
            std::memcpy(buf.data() + 40, &alloc, 8);
            std::memcpy(buf.data() + 48, &eof, 8);
            std::memcpy(buf.data() + 56, &nl, 4);
            std::memcpy(buf.data() + 60, &del_pending, 1);
            std::memcpy(buf.data() + 61, &is_dir, 1);
            std::memcpy(buf.data() + 62, &ino, 8);
            break;
        }
        default:
            return std::nullopt;
        }

        return buf;
    }

    NtStatus LocalFileSystem::set_info(void * handle, FileInfoClass info_class, const uint8_t * data, uint32_t len)
    {
        if (!handle)
            return NtStatus::INVALID_HANDLE;

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));

        switch (info_class) {
        case FileInfoClass::FileEndOfFileInformation: {
            if (len < 8)
                return NtStatus::INVALID_PARAMETER;
            uint64_t new_size;
            std::memcpy(&new_size, data, 8);
            if (ftruncate64(fd, static_cast<off64_t>(new_size)) != 0) {
                return NtStatus::UNSUCCESSFUL;
            }
            return NtStatus::SUCCESS;
        }
        case FileInfoClass::FileDispositionInformation: {
            return NtStatus::SUCCESS;
        }
        case FileInfoClass::FileBasicInformation: {
            struct timespec ts[2];
            if (len >= 24) {
                uint64_t at, wt;
                std::memcpy(&at, data + 8, 8);
                std::memcpy(&wt, data + 16, 8);
                if (at != 0) {
                    int64_t unix_ns = (static_cast<int64_t>(at) - static_cast<int64_t>(EPOCH_DIFFERENCE)) * 100;
                    ts[0].tv_sec = static_cast<time_t>(unix_ns / 1000000000);
                    ts[0].tv_nsec = static_cast<long>(unix_ns % 1000000000);
                } else {
                    ts[0].tv_sec = 0;
                    ts[0].tv_nsec = UTIME_OMIT;
                }
                if (wt != 0) {
                    int64_t unix_ns = (static_cast<int64_t>(wt) - static_cast<int64_t>(EPOCH_DIFFERENCE)) * 100;
                    ts[1].tv_sec = static_cast<time_t>(unix_ns / 1000000000);
                    ts[1].tv_nsec = static_cast<long>(unix_ns % 1000000000);
                } else {
                    ts[1].tv_sec = 0;
                    ts[1].tv_nsec = UTIME_OMIT;
                }
                futimens(fd, ts);
            }
            return NtStatus::SUCCESS;
        }
        case FileInfoClass::FileRenameInformation: {
            return NtStatus::SUCCESS;
        }
        default:
            return NtStatus::NOT_SUPPORTED;
        }
    }

    static bool wildcard_match(const std::string & pattern, const std::string & name)
    {
        if (pattern == "*" || pattern == "*.*") {
            return true;
        }

        size_t pi = 0, ni = 0;
        while (pi < pattern.size() && ni < name.size()) {
            if (pattern[pi] == '?') {
                pi++;
                ni++;
            } else if (pattern[pi] == '*') {
                pi++;
                if (pi == pattern.size())
                    return true;
                while (ni < name.size()) {
                    if (wildcard_match(pattern.substr(pi), name.substr(ni))) {
                        return true;
                    }
                    ni++;
                }
                return false;
            } else if (pattern[pi] == name[ni]) {
                pi++;
                ni++;
            } else {
                return false;
            }
        }

        while (pi < pattern.size() && pattern[pi] == '*')
            pi++;
        return pi == pattern.size() && ni == name.size();
    }

    std::optional<std::vector<DirEntry> > LocalFileSystem::query_directory(void * handle, const std::string & pattern,
                                                                           FileInfoClass info_class, bool restart)
    {
        if (!handle)
            return std::nullopt;

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        std::string fd_path;
        {
            char link[PATH_MAX];
            std::string link_path = "/proc/self/fd/" + std::to_string(fd);
            ssize_t n = readlink(link_path.c_str(), link, sizeof(link) - 1);
            if (n < 0)
                return std::nullopt;
            link[n] = '\0';
            fd_path = link;
        }

        DIR *dir = opendir(fd_path.c_str());
        if (!dir)
            return std::nullopt;

        std::vector<DirEntry> entries;
        struct dirent *entry;
        std::string pat = pattern.empty() ? "*" : pattern;

        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string name_str(entry->d_name);
            if (!wildcard_match(pat, name_str)) {
                continue;
            }

            std::string full_path = fd_path + "/" + name_str;
            struct stat st;
            if (stat(full_path.c_str(), &st) != 0) {
                continue;
            }

            DirEntry de;
            de.creation_time = filetime_from_timespec(st.st_ctim);
            de.last_access_time = filetime_from_timespec(st.st_atim);
            de.last_write_time = filetime_from_timespec(st.st_mtim);
            de.change_time = filetime_from_timespec(st.st_ctim);
            de.end_of_file = static_cast<uint64_t>(st.st_size);
            de.allocation_size = static_cast<uint64_t>(st.st_blocks) * 512;
            de.file_attributes = posix_to_file_attributes(st.st_mode);
            de.file_id.persistent = static_cast<uint64_t>(st.st_ino);
            de.file_id.volatile_id = de.file_id.persistent;

            for (char c : name_str) {
                de.file_name.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
            }

            entries.push_back(std::move(de));
        }

        closedir(dir);
        return entries;
    }

    NtStatus LocalFileSystem::rename(void * handle, const std::string & new_path, bool replace)
    {
        if (!handle)
            return NtStatus::INVALID_HANDLE;

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        char link[PATH_MAX];
        std::string link_path = "/proc/self/fd/" + std::to_string(fd);
        ssize_t n = readlink(link_path.c_str(), link, sizeof(link) - 1);
        if (n < 0)
            return NtStatus::UNSUCCESSFUL;
        link[n] = '\0';

        std::string dest = resolve(new_path);
        if (!replace) {
            struct stat st;
            if (stat(dest.c_str(), &st) == 0) {
                return NtStatus::OBJECT_NAME_COLLISION;
            }
        }

        if (::rename(link, dest.c_str()) != 0) {
            return NtStatus::UNSUCCESSFUL;
        }
        return NtStatus::SUCCESS;
    }

    NtStatus LocalFileSystem::delete_file(void * handle)
    {
        if (!handle)
            return NtStatus::INVALID_HANDLE;

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        char link[PATH_MAX];
        std::string link_path = "/proc/self/fd/" + std::to_string(fd);
        ssize_t n = readlink(link_path.c_str(), link, sizeof(link) - 1);
        if (n < 0)
            return NtStatus::UNSUCCESSFUL;
        link[n] = '\0';

        struct stat st;
        if (fstat(fd, &st) != 0)
            return NtStatus::UNSUCCESSFUL;

        ::close(fd);

        int ret;
        if (S_ISDIR(st.st_mode)) {
            ret = ::rmdir(link);
        } else {
            ret = ::unlink(link);
        }

        if (ret != 0)
            return NtStatus::UNSUCCESSFUL;
        return NtStatus::SUCCESS;
    }

    NtStatus LocalFileSystem::flush(void * handle)
    {
        if (!handle)
            return NtStatus::INVALID_HANDLE;
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        if (fsync(fd) != 0)
            return NtStatus::UNSUCCESSFUL;
        return NtStatus::SUCCESS;
    }

    LockResult LocalFileSystem::lock(void * handle, uint64_t offset, uint64_t length, bool exclusive)
    {
        LockResult result;
        if (!handle) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        struct flock fl;
        std::memset(&fl, 0, sizeof(fl));
        fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = static_cast<off_t>(offset);
        fl.l_len = static_cast<off_t>(length);

        if (fcntl(fd, F_SETLK, &fl) != 0) {
            result.success = false;
            if (errno == EAGAIN || errno == EACCES) {
                result.status = NtStatus::LOCK_NOT_GRANTED;
            } else {
                result.status = NtStatus::UNSUCCESSFUL;
            }
            return result;
        }

        result.success = true;
        result.status = NtStatus::SUCCESS;
        return result;
    }

    NtStatus LocalFileSystem::unlock(void * handle, uint64_t offset, uint64_t length)
    {
        if (!handle)
            return NtStatus::INVALID_HANDLE;

        int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle));
        struct flock fl;
        std::memset(&fl, 0, sizeof(fl));
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = static_cast<off_t>(offset);
        fl.l_len = static_cast<off_t>(length);

        if (fcntl(fd, F_SETLK, &fl) != 0) {
            return NtStatus::UNSUCCESSFUL;
        }
        return NtStatus::SUCCESS;
    }

    IoctlResult LocalFileSystem::fsctl(void * handle, uint32_t code, const uint8_t * input, uint32_t input_len)
    {
        IoctlResult result;
        result.success = false;
        result.status = NtStatus::NOT_SUPPORTED;
        return result;
    }
}
