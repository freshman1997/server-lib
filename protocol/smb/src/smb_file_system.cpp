#include "smb_file_system.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace yuan::net::smb
{
    namespace
    {
        struct LocalHandle
        {
#ifdef _WIN32
            HANDLE handle = INVALID_HANDLE_VALUE;
#else
            int fd = -1;
#endif
            std::string path;
            bool is_directory = false;
        };

        static constexpr uint64_t EPOCH_DIFFERENCE = 116444736000000000ULL;

        static LocalHandle *to_handle(void *handle)
        {
            return static_cast<LocalHandle *>(handle);
        }

        static uint64_t unix_time_to_filetime(std::time_t sec, long nsec = 0)
        {
            uint64_t result = EPOCH_DIFFERENCE;
            result += static_cast<uint64_t>(sec) * 10000000ULL;
            result += static_cast<uint64_t>(nsec) / 100ULL;
            return result;
        }

#ifdef _WIN32
        static uint64_t filetime_to_nt(const FILETIME &ft)
        {
            ULARGE_INTEGER uli{};
            uli.LowPart = ft.dwLowDateTime;
            uli.HighPart = ft.dwHighDateTime;
            return uli.QuadPart;
        }

        static FILETIME nt_to_filetime(uint64_t ft)
        {
            ULARGE_INTEGER uli{};
            uli.QuadPart = ft;
            FILETIME ret{};
            ret.dwLowDateTime = uli.LowPart;
            ret.dwHighDateTime = uli.HighPart;
            return ret;
        }
#endif

        static bool wildcard_match(const std::string &pattern, const std::string &name)
        {
            if (pattern == "*" || pattern == "*.*") {
                return true;
            }

            size_t pi = 0;
            size_t ni = 0;
            while (pi < pattern.size() && ni < name.size()) {
                if (pattern[pi] == '?') {
                    ++pi;
                    ++ni;
                } else if (pattern[pi] == '*') {
                    ++pi;
                    if (pi == pattern.size()) {
                        return true;
                    }
                    while (ni < name.size()) {
                        if (wildcard_match(pattern.substr(pi), name.substr(ni))) {
                            return true;
                        }
                        ++ni;
                    }
                    return false;
                } else if (pattern[pi] == name[ni]) {
                    ++pi;
                    ++ni;
                } else {
                    return false;
                }
            }

            while (pi < pattern.size() && pattern[pi] == '*') {
                ++pi;
            }
            return pi == pattern.size() && ni == name.size();
        }
    }

    LocalFileSystem::LocalFileSystem(const std::string &root_path)
        : root_path_(root_path)
    {
        if (!root_path_.empty() && root_path_.back() == '/') {
            root_path_.pop_back();
        }
    }

    LocalFileSystem::~LocalFileSystem() = default;

    std::string LocalFileSystem::resolve(const std::string &relative) const
    {
        std::string normalized = relative;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        while (!normalized.empty() && normalized.front() == '/') {
            normalized.erase(normalized.begin());
        }

        namespace fs = std::filesystem;
        fs::path root(root_path_);
        fs::path combined = root / normalized;
        fs::path canonical = fs::weakly_canonical(combined);
        fs::path canonical_root = fs::weakly_canonical(root);

        auto root_str = canonical_root.string();
        auto canon_str = canonical.string();
        if (canon_str.size() < root_str.size() || canon_str.substr(0, root_str.size()) != root_str) {
            return root_path_;
        }
        return canonical.string();
    }

    uint64_t LocalFileSystem::filetime_now()
    {
#ifdef _WIN32
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return filetime_to_nt(ft);
#else
        struct timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return unix_time_to_filetime(ts.tv_sec, ts.tv_nsec);
#endif
    }

    uint64_t LocalFileSystem::filetime_from_unix_time(std::time_t sec, long nsec)
    {
        return unix_time_to_filetime(sec, nsec);
    }

    uint32_t LocalFileSystem::posix_to_file_attributes(unsigned int mode) const
    {
        uint32_t attrs = 0;
        if (mode & S_IFDIR) {
            attrs |= FILE_ATTRIBUTE_DIRECTORY;
        } else {
            attrs |= FILE_ATTRIBUTE_NORMAL;
        }
        if (!(mode & S_IWUSR)) {
            attrs |= FILE_ATTRIBUTE_READONLY;
        }
        return attrs;
    }

    OpenResult LocalFileSystem::open(const std::string &path, uint32_t desired_access,
                                     uint32_t create_disposition, uint32_t create_options)
    {
        OpenResult result;
        std::string full_path = resolve(path);
        auto *h = new LocalHandle();
        h->path = full_path;
        h->is_directory = (create_options & FILE_DIRECTORY_FILE) != 0;

#ifdef _WIN32
        DWORD access = 0;
        if (desired_access & (GENERIC_READ | FILE_READ_DATA)) {
            access |= GENERIC_READ;
        }
        if (desired_access & (GENERIC_WRITE | FILE_WRITE_DATA)) {
            access |= GENERIC_WRITE;
        }
        if (access == 0) {
            access = GENERIC_READ;
        }

        DWORD creation = OPEN_EXISTING;
        switch (create_disposition) {
        case FILE_SUPERSEDE:
        case FILE_OVERWRITE:
            creation = CREATE_ALWAYS;
            break;
        case FILE_OVERWRITE_IF:
            creation = OPEN_ALWAYS;
            break;
        case FILE_CREATE:
            creation = CREATE_NEW;
            break;
        case FILE_OPEN:
            creation = OPEN_EXISTING;
            break;
        case FILE_OPEN_IF:
            creation = OPEN_ALWAYS;
            break;
        default:
            break;
        }

        DWORD attrs = FILE_ATTRIBUTE_NORMAL;
        if (h->is_directory) {
            attrs |= FILE_FLAG_BACKUP_SEMANTICS;
        }

        HANDLE handle = CreateFileA(full_path.c_str(), access,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, creation, attrs, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            delete h;
            result.success = false;
            switch (GetLastError()) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                result.status = NtStatus::OBJECT_NAME_NOT_FOUND;
                break;
            case ERROR_ALREADY_EXISTS:
            case ERROR_FILE_EXISTS:
                result.status = NtStatus::OBJECT_NAME_COLLISION;
                break;
            case ERROR_ACCESS_DENIED:
                result.status = NtStatus::ACCESS_DENIED;
                break;
            default:
                result.status = NtStatus::UNSUCCESSFUL;
                break;
            }
            return result;
        }

        h->handle = handle;
        result.handle = h;

        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(handle, &info)) {
            result.is_directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            result.allocation_size = static_cast<uint64_t>(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
            result.end_of_file = result.allocation_size;
            result.file_attributes = info.dwFileAttributes;
            result.creation_time = filetime_to_nt(info.ftCreationTime);
            result.last_access_time = filetime_to_nt(info.ftLastAccessTime);
            result.last_write_time = filetime_to_nt(info.ftLastWriteTime);
            result.change_time = result.last_write_time;
            result.create_action = (create_disposition == FILE_CREATE) ? FILE_CREATED : FILE_OPENED;
        } else {
            result.create_action = FILE_OPENED;
        }
        result.success = true;
        result.status = NtStatus::SUCCESS;
        return result;
#else
        int flags = 0;
        if (desired_access & (GENERIC_WRITE | FILE_WRITE_DATA)) {
            if (desired_access & (GENERIC_READ | FILE_READ_DATA)) {
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

        if (h->is_directory) {
            delete h;
            result.success = false;
            result.status = NtStatus::NOT_SUPPORTED;
            return result;
        }

        int fd = ::open(full_path.c_str(), flags, 0644);
        if (fd < 0) {
            delete h;
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

        h->fd = fd;
        result.handle = h;

        struct stat st{};
        if (fstat(fd, &st) == 0) {
            result.is_directory = S_ISDIR(st.st_mode);
            result.allocation_size = static_cast<uint64_t>(st.st_blocks) * 512ULL;
            result.end_of_file = static_cast<uint64_t>(st.st_size);
            result.file_attributes = posix_to_file_attributes(static_cast<unsigned int>(st.st_mode));
            result.creation_time = filetime_from_unix_time(st.st_ctime);
            result.last_access_time = filetime_from_unix_time(st.st_atime);
            result.last_write_time = filetime_from_unix_time(st.st_mtime);
            result.change_time = result.creation_time;
        }
        result.create_action = (create_disposition == FILE_CREATE) ? FILE_CREATED : FILE_OPENED;
        result.success = true;
        result.status = NtStatus::SUCCESS;
        return result;
#endif
    }

    void LocalFileSystem::close(void *handle)
    {
        auto *h = to_handle(handle);
        if (!h) {
            return;
        }
#ifdef _WIN32
        if (h->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(h->handle);
            h->handle = INVALID_HANDLE_VALUE;
        }
#else
        if (h->fd >= 0) {
            ::close(h->fd);
            h->fd = -1;
        }
#endif
        delete h;
    }

    ReadResult LocalFileSystem::read(void *handle, uint64_t offset, uint32_t length)
    {
        ReadResult result;
        auto *h = to_handle(handle);
        if (!h) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }

        result.data.resize(length);
#ifdef _WIN32
        if (h->handle == INVALID_HANDLE_VALUE) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(h->handle, li, nullptr, FILE_BEGIN)) {
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }
        DWORD bytes_read = 0;
        if (!ReadFile(h->handle, result.data.data(), length, &bytes_read, nullptr)) {
            result.data.clear();
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }
        result.data.resize(bytes_read);
        result.bytes_read = bytes_read;
#else
        if (h->fd < 0) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }
        ssize_t n = pread(h->fd, result.data.data(), length, static_cast<off_t>(offset));
        if (n < 0) {
            result.data.clear();
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }
        result.data.resize(static_cast<size_t>(n));
        result.bytes_read = static_cast<uint32_t>(n);
#endif
        result.success = true;
        result.status = NtStatus::SUCCESS;
        return result;
    }

    WriteResult LocalFileSystem::write(void *handle, uint64_t offset, const uint8_t *data, uint32_t length)
    {
        WriteResult result;
        auto *h = to_handle(handle);
        if (!h) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }

#ifdef _WIN32
        if (h->handle == INVALID_HANDLE_VALUE) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(h->handle, li, nullptr, FILE_BEGIN)) {
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }
        DWORD bytes_written = 0;
        if (!WriteFile(h->handle, data, length, &bytes_written, nullptr)) {
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }
        result.bytes_written = bytes_written;
#else
        if (h->fd < 0) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }
        ssize_t n = pwrite(h->fd, data, length, static_cast<off_t>(offset));
        if (n < 0) {
            result.status = NtStatus::UNSUCCESSFUL;
            return result;
        }
        result.bytes_written = static_cast<uint32_t>(n);
#endif
        result.success = true;
        result.status = NtStatus::SUCCESS;
        return result;
    }

    std::optional<std::vector<uint8_t> > LocalFileSystem::query_info(void *handle, FileInfoClass info_class)
    {
        auto *h = to_handle(handle);
        if (!h) {
            return std::nullopt;
        }

        std::vector<uint8_t> buf;

#ifdef _WIN32
        if (h->handle == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h->handle, &info)) {
            return std::nullopt;
        }

        const uint64_t ct = filetime_to_nt(info.ftCreationTime);
        const uint64_t at = filetime_to_nt(info.ftLastAccessTime);
        const uint64_t wt = filetime_to_nt(info.ftLastWriteTime);
        const uint32_t attrs = info.dwFileAttributes;
        const uint64_t alloc = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const uint64_t eof = alloc;
        const uint32_t nl = info.nNumberOfLinks;
        const uint8_t del_pending = 0;
        const uint8_t is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        const uint64_t ino = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
#else
        if (h->fd < 0) {
            return std::nullopt;
        }
        struct stat st{};
        if (fstat(h->fd, &st) != 0) {
            return std::nullopt;
        }
        const uint64_t ct = filetime_from_unix_time(st.st_ctime);
        const uint64_t at = filetime_from_unix_time(st.st_atime);
        const uint64_t wt = filetime_from_unix_time(st.st_mtime);
        const uint32_t attrs = posix_to_file_attributes(static_cast<unsigned int>(st.st_mode));
        const uint64_t alloc = static_cast<uint64_t>(st.st_blocks) * 512ULL;
        const uint64_t eof = static_cast<uint64_t>(st.st_size);
        const uint32_t nl = static_cast<uint32_t>(st.st_nlink);
        const uint8_t del_pending = 0;
        const uint8_t is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        const uint64_t ino = static_cast<uint64_t>(st.st_ino);
#endif

        switch (info_class) {
        case FileInfoClass::FileBasicInformation: {
            buf.resize(40);
            std::memcpy(buf.data() + 0, &ct, 8);
            std::memcpy(buf.data() + 8, &at, 8);
            std::memcpy(buf.data() + 16, &wt, 8);
            std::memcpy(buf.data() + 24, &ct, 8);
            std::memcpy(buf.data() + 32, &attrs, 4);
            std::memset(buf.data() + 36, 0, 4);
            break;
        }
        case FileInfoClass::FileStandardInformation: {
            buf.resize(24);
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
            std::memcpy(buf.data(), &ino, 8);
            break;
        }
        case FileInfoClass::FileAllInformation: {
            buf.resize(70);
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

    NtStatus LocalFileSystem::set_info(void *handle, FileInfoClass info_class, const uint8_t *data, uint32_t len)
    {
        auto *h = to_handle(handle);
        if (!h) {
            return NtStatus::INVALID_HANDLE;
        }

        switch (info_class) {
        case FileInfoClass::FileEndOfFileInformation: {
            if (len < 8) {
                return NtStatus::INVALID_PARAMETER;
            }
            uint64_t new_size = 0;
            std::memcpy(&new_size, data, 8);
#ifdef _WIN32
            if (h->handle == INVALID_HANDLE_VALUE) {
                return NtStatus::INVALID_HANDLE;
            }
            LARGE_INTEGER li{};
            li.QuadPart = static_cast<LONGLONG>(new_size);
            if (!SetFilePointerEx(h->handle, li, nullptr, FILE_BEGIN) || !SetEndOfFile(h->handle)) {
                return NtStatus::UNSUCCESSFUL;
            }
#else
            if (h->fd < 0 || ftruncate(h->fd, static_cast<off_t>(new_size)) != 0) {
                return NtStatus::UNSUCCESSFUL;
            }
#endif
            return NtStatus::SUCCESS;
        }
        case FileInfoClass::FileDispositionInformation:
            return NtStatus::SUCCESS;
        case FileInfoClass::FileBasicInformation:
            return NtStatus::SUCCESS;
        case FileInfoClass::FileRenameInformation:
            return NtStatus::SUCCESS;
        default:
            return NtStatus::NOT_SUPPORTED;
        }
    }

    std::optional<std::vector<DirEntry> > LocalFileSystem::query_directory(void *handle, const std::string &pattern,
                                                                           FileInfoClass info_class, bool restart)
    {
        (void)info_class;
        (void)restart;
        auto *h = to_handle(handle);
        if (!h) {
            return std::nullopt;
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir_path(h->path);
        if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
            return std::nullopt;
        }

        std::vector<DirEntry> entries;
        const std::string pat = pattern.empty() ? "*" : pattern;
        for (const auto &entry : fs::directory_iterator(dir_path, ec)) {
            if (ec) {
                break;
            }
            const auto name = entry.path().filename().string();
            if (!wildcard_match(pat, name)) {
                continue;
            }

            std::error_code st_ec;
            auto status = entry.symlink_status(st_ec);
            auto fsize = entry.is_regular_file(st_ec) ? fs::file_size(entry.path(), st_ec) : 0ULL;

            DirEntry de;
            de.file_name.reserve(name.size());
            for (char c : name) {
                de.file_name.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
            }
            de.end_of_file = fsize;
            de.allocation_size = fsize;
            de.file_attributes = entry.is_directory(st_ec) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            de.file_id.persistent = static_cast<uint64_t>(std::hash<std::string>{}(entry.path().string()));
            de.file_id.volatile_id = de.file_id.persistent;
            if (status.type() == fs::file_type::symlink) {
                de.file_attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
            }

            const uint64_t ft = filetime_now();
            de.creation_time = ft;
            de.last_access_time = ft;
            de.last_write_time = ft;
            de.change_time = ft;

            entries.push_back(std::move(de));
        }

        return entries;
    }

    NtStatus LocalFileSystem::rename(void *handle, const std::string &new_path, bool replace)
    {
        auto *h = to_handle(handle);
        if (!h) {
            return NtStatus::INVALID_HANDLE;
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dest = resolve(new_path);
        if (!replace && fs::exists(dest, ec)) {
            return NtStatus::OBJECT_NAME_COLLISION;
        }

        fs::rename(fs::path(h->path), dest, ec);
        if (ec) {
            return NtStatus::UNSUCCESSFUL;
        }
        h->path = dest.string();
        return NtStatus::SUCCESS;
    }

    NtStatus LocalFileSystem::delete_file(void *handle)
    {
        auto *h = to_handle(handle);
        if (!h) {
            return NtStatus::INVALID_HANDLE;
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path p(h->path);
        bool removed = false;
        if (h->is_directory || fs::is_directory(p, ec)) {
            removed = fs::remove_all(p, ec) > 0;
        } else {
            removed = fs::remove(p, ec);
        }
        if (!removed || ec) {
            return NtStatus::UNSUCCESSFUL;
        }
        return NtStatus::SUCCESS;
    }

    NtStatus LocalFileSystem::flush(void *handle)
    {
        auto *h = to_handle(handle);
        if (!h) {
            return NtStatus::INVALID_HANDLE;
        }
#ifdef _WIN32
        if (h->handle == INVALID_HANDLE_VALUE) {
            return NtStatus::INVALID_HANDLE;
        }
        return FlushFileBuffers(h->handle) ? NtStatus::SUCCESS : NtStatus::UNSUCCESSFUL;
#else
        if (h->fd < 0) {
            return NtStatus::INVALID_HANDLE;
        }
        return (::fsync(h->fd) == 0) ? NtStatus::SUCCESS : NtStatus::UNSUCCESSFUL;
#endif
    }

    LockResult LocalFileSystem::lock(void *handle, uint64_t offset, uint64_t length, bool exclusive)
    {
        LockResult result;
        auto *h = to_handle(handle);
        if (!h) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }
#ifdef _WIN32
        if (h->handle == INVALID_HANDLE_VALUE) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFULL);
        DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
        if (!LockFileEx(h->handle, flags, 0, static_cast<DWORD>(length & 0xFFFFFFFFULL),
                        static_cast<DWORD>((length >> 32) & 0xFFFFFFFFULL), &ov)) {
            result.status = NtStatus::LOCK_NOT_GRANTED;
            return result;
        }
#else
        if (h->fd < 0) {
            result.status = NtStatus::INVALID_HANDLE;
            return result;
        }
        struct flock fl{};
        fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = static_cast<off_t>(offset);
        fl.l_len = static_cast<off_t>(length);
        if (fcntl(h->fd, F_SETLK, &fl) != 0) {
            result.status = (errno == EAGAIN || errno == EACCES) ? NtStatus::LOCK_NOT_GRANTED : NtStatus::UNSUCCESSFUL;
            return result;
        }
#endif
        result.success = true;
        result.status = NtStatus::SUCCESS;
        return result;
    }

    NtStatus LocalFileSystem::unlock(void *handle, uint64_t offset, uint64_t length)
    {
        auto *h = to_handle(handle);
        if (!h) {
            return NtStatus::INVALID_HANDLE;
        }
#ifdef _WIN32
        if (h->handle == INVALID_HANDLE_VALUE) {
            return NtStatus::INVALID_HANDLE;
        }
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFULL);
        if (!UnlockFileEx(h->handle, 0, static_cast<DWORD>(length & 0xFFFFFFFFULL),
                          static_cast<DWORD>((length >> 32) & 0xFFFFFFFFULL), &ov)) {
            return NtStatus::UNSUCCESSFUL;
        }
#else
        if (h->fd < 0) {
            return NtStatus::INVALID_HANDLE;
        }
        struct flock fl{};
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = static_cast<off_t>(offset);
        fl.l_len = static_cast<off_t>(length);
        if (fcntl(h->fd, F_SETLK, &fl) != 0) {
            return NtStatus::UNSUCCESSFUL;
        }
#endif
        return NtStatus::SUCCESS;
    }

    IoctlResult LocalFileSystem::fsctl(void *handle, uint32_t code, const uint8_t *input, uint32_t input_len)
    {
        (void)handle;
        (void)code;
        (void)input;
        (void)input_len;
        IoctlResult result;
        result.success = false;
        result.status = NtStatus::NOT_SUPPORTED;
        return result;
    }
}
