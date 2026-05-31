#include "webdav_resource.h"
#include "webdav_xml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <cwctype>
#include <windows.h>
#endif

namespace yuan::net::webdav
{
    namespace
    {
        std::string clean_href(std::string_view href)
        {
            std::string h(href);
            const auto q = h.find('?');
            if (q != std::string::npos) {
                h.resize(q);
            }
            while (!h.empty() && h.front() == '/') {
                h.erase(h.begin());
            }
            return h;
        }

        std::string extension_content_type(const std::filesystem::path &path)
        {
#ifdef _WIN32
            auto ext = path.extension().native();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            if (ext == L".html" || ext == L".htm") return "text/html; charset=utf-8";
            if (ext == L".txt" || ext == L".log" || ext == L".md" || ext == L".ini" || ext == L".conf" || ext == L".cmake") return "text/plain; charset=utf-8";
            if (ext == L".csv") return "text/csv; charset=utf-8";
            if (ext == L".css") return "text/css; charset=utf-8";
            if (ext == L".js" || ext == L".mjs") return "text/javascript; charset=utf-8";
            if (ext == L".json") return "application/json";
            if (ext == L".xml") return "application/xml";
            if (ext == L".pdf") return "application/pdf";
            if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
            if (ext == L".png") return "image/png";
            if (ext == L".gif") return "image/gif";
            if (ext == L".webp") return "image/webp";
            if (ext == L".bmp") return "image/bmp";
            if (ext == L".svg") return "image/svg+xml";
            if (ext == L".ico") return "image/x-icon";
            if (ext == L".tif" || ext == L".tiff") return "image/tiff";
            if (ext == L".mp4" || ext == L".m4v") return "video/mp4";
            if (ext == L".webm") return "video/webm";
            if (ext == L".ogv") return "video/ogg";
            if (ext == L".mov") return "video/quicktime";
            if (ext == L".mp3") return "audio/mpeg";
            if (ext == L".wav") return "audio/wav";
            if (ext == L".ogg" || ext == L".oga") return "audio/ogg";
            if (ext == L".flac") return "audio/flac";
            if (ext == L".aac") return "audio/aac";
            if (ext == L".m4a") return "audio/mp4";
            if (ext == L".opus") return "audio/opus";
            if (ext == L".zip") return "application/zip";
            if (ext == L".7z") return "application/x-7z-compressed";
            if (ext == L".rar") return "application/vnd.rar";
            if (ext == L".tar") return "application/x-tar";
            if (ext == L".gz") return "application/gzip";
            if (ext == L".bz2") return "application/x-bzip2";
            if (ext == L".xz") return "application/x-xz";
            if (ext == L".doc") return "application/msword";
            if (ext == L".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
            if (ext == L".xls") return "application/vnd.ms-excel";
            if (ext == L".xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
            if (ext == L".ppt") return "application/vnd.ms-powerpoint";
            if (ext == L".pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
            if (ext == L".wasm") return "application/wasm";
            if (ext == L".woff") return "font/woff";
            if (ext == L".woff2") return "font/woff2";
            if (ext == L".ttf") return "font/ttf";
            if (ext == L".otf") return "font/otf";
#else
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
            if (ext == ".txt" || ext == ".log" || ext == ".md" || ext == ".ini" || ext == ".conf" || ext == ".cmake") return "text/plain; charset=utf-8";
            if (ext == ".csv") return "text/csv; charset=utf-8";
            if (ext == ".css") return "text/css; charset=utf-8";
            if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
            if (ext == ".json") return "application/json";
            if (ext == ".xml") return "application/xml";
            if (ext == ".pdf") return "application/pdf";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".png") return "image/png";
            if (ext == ".gif") return "image/gif";
            if (ext == ".webp") return "image/webp";
            if (ext == ".bmp") return "image/bmp";
            if (ext == ".svg") return "image/svg+xml";
            if (ext == ".ico") return "image/x-icon";
            if (ext == ".tif" || ext == ".tiff") return "image/tiff";
            if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
            if (ext == ".webm") return "video/webm";
            if (ext == ".ogv") return "video/ogg";
            if (ext == ".mov") return "video/quicktime";
            if (ext == ".mp3") return "audio/mpeg";
            if (ext == ".wav") return "audio/wav";
            if (ext == ".ogg" || ext == ".oga") return "audio/ogg";
            if (ext == ".flac") return "audio/flac";
            if (ext == ".aac") return "audio/aac";
            if (ext == ".m4a") return "audio/mp4";
            if (ext == ".opus") return "audio/opus";
            if (ext == ".zip") return "application/zip";
            if (ext == ".7z") return "application/x-7z-compressed";
            if (ext == ".rar") return "application/vnd.rar";
            if (ext == ".tar") return "application/x-tar";
            if (ext == ".gz") return "application/gzip";
            if (ext == ".bz2") return "application/x-bzip2";
            if (ext == ".xz") return "application/x-xz";
            if (ext == ".doc") return "application/msword";
            if (ext == ".docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
            if (ext == ".xls") return "application/vnd.ms-excel";
            if (ext == ".xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
            if (ext == ".ppt") return "application/vnd.ms-powerpoint";
            if (ext == ".pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
            if (ext == ".wasm") return "application/wasm";
            if (ext == ".woff") return "font/woff";
            if (ext == ".woff2") return "font/woff2";
            if (ext == ".ttf") return "font/ttf";
            if (ext == ".otf") return "font/otf";
#endif
            return "application/octet-stream";
        }

        std::filesystem::path path_from_utf8(std::string_view text)
        {
#ifdef _WIN32
            if (text.empty()) {
                return {};
            }
            const int wide_len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (wide_len <= 0) {
                return std::filesystem::path(std::string(text));
            }
            std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), wide_len);
            return std::filesystem::path(wide);
#else
            return std::filesystem::path(std::string(text));
#endif
        }

        std::string path_to_utf8(const std::filesystem::path &path)
        {
#ifdef _WIN32
            const auto wide = path.native();
            if (wide.empty()) {
                return {};
            }
            const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (utf8_len <= 0) {
                return {};
            }
            std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8_len, nullptr, nullptr);
            return utf8;
#else
            return path.string();
#endif
        }

        std::chrono::system_clock::time_point file_time_to_sys(std::filesystem::file_time_type ft)
        {
            return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        }
    }

    LocalWebDavBackend::LocalWebDavBackend(std::filesystem::path root)
        : root_(std::filesystem::absolute(std::move(root))),
          property_store_(root_ / ".webdav-properties")
    {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        std::filesystem::create_directories(property_store_, ec);
    }

    std::optional<std::filesystem::path> WebDavResourceBackend::file_path_for_read(std::string_view) const
    {
        return std::nullopt;
    }

    BackendResult WebDavResourceBackend::write_from_file(std::string_view href,
                                                         const std::filesystem::path &source,
                                                         bool create_parents)
    {
        std::ifstream in(source, std::ios::binary);
        if (!in.good()) {
            return BackendResult::failure(404);
        }
        std::ostringstream oss;
        oss << in.rdbuf();
        return write(href, oss.str(), create_parents);
    }

    ResourceInfo LocalWebDavBackend::stat(std::string_view href) const
    {
        return stat_path(resolve(href));
    }

    std::vector<ChildResource> LocalWebDavBackend::list(std::string_view href) const
    {
        std::vector<ChildResource> out;
        const auto path = resolve(href);
        std::error_code ec;
        if (!contains_root(path) || !std::filesystem::is_directory(path, ec)) {
            return out;
        }
        for (const auto &entry : std::filesystem::directory_iterator(path, ec)) {
            if (entry.path() == property_store_) {
                continue;
            }
            ChildResource child;
            child.name = path_to_utf8(entry.path().filename());
            child.info = stat_path(entry.path());
            out.push_back(std::move(child));
        }
        return out;
    }

    std::optional<std::string> LocalWebDavBackend::read(std::string_view href) const
    {
        const auto path = resolve(href);
        std::error_code ec;
        if (!contains_root(path) || !std::filesystem::is_regular_file(path, ec)) {
            return std::nullopt;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            return std::nullopt;
        }
        std::ostringstream oss;
        oss << in.rdbuf();
        return oss.str();
    }

    std::optional<std::filesystem::path> LocalWebDavBackend::file_path_for_read(std::string_view href) const
    {
        const auto path = resolve(href);
        std::error_code ec;
        if (!contains_root(path) || !std::filesystem::is_regular_file(path, ec)) {
            return std::nullopt;
        }
        return path;
    }

    BackendResult LocalWebDavBackend::write(std::string_view href, std::string_view body, bool create_parents)
    {
        const auto path = resolve(href);
        if (!contains_root(path)) {
            return BackendResult::failure(403);
        }
        std::error_code ec;
        const bool existed = std::filesystem::exists(path, ec);
        if (create_parents) {
            std::filesystem::create_directories(path.parent_path(), ec);
        } else if (!std::filesystem::exists(path.parent_path(), ec)) {
            return BackendResult::failure(409);
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return BackendResult::failure(507);
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        return BackendResult::success(existed ? 204 : 201);
    }

    BackendResult LocalWebDavBackend::write_from_file(std::string_view href,
                                                      const std::filesystem::path &source,
                                                      bool create_parents)
    {
        const auto path = resolve(href);
        if (!contains_root(path)) {
            return BackendResult::failure(403);
        }
        std::error_code ec;
        const bool existed = std::filesystem::exists(path, ec);
        if (create_parents) {
            std::filesystem::create_directories(path.parent_path(), ec);
        } else if (!std::filesystem::exists(path.parent_path(), ec)) {
            return BackendResult::failure(409);
        }
        if (!std::filesystem::is_regular_file(source, ec)) {
            return BackendResult::failure(404);
        }
        std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing, ec);
        return ec ? BackendResult::failure(507, ec.message())
                  : BackendResult::success(existed ? 204 : 201);
    }

    BackendResult LocalWebDavBackend::make_collection(std::string_view href)
    {
        const auto path = resolve(href);
        if (!contains_root(path)) {
            return BackendResult::failure(403);
        }
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return BackendResult::failure(405);
        }
        if (!std::filesystem::exists(path.parent_path(), ec)) {
            return BackendResult::failure(409);
        }
        if (!std::filesystem::create_directory(path, ec)) {
            return BackendResult::failure(ec ? 507 : 405);
        }
        return BackendResult::success(201);
    }

    BackendResult LocalWebDavBackend::remove(std::string_view href)
    {
        const auto path = resolve(href);
        if (!contains_root(path)) {
            return BackendResult::failure(403);
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return BackendResult::failure(404);
        }
        std::filesystem::remove_all(path, ec);
        return ec ? BackendResult::failure(500, ec.message()) : BackendResult::success(204);
    }

    BackendResult LocalWebDavBackend::copy(std::string_view from, std::string_view to, bool overwrite)
    {
        const auto src = resolve(from);
        const auto dst = resolve(to);
        if (!contains_root(src) || !contains_root(dst)) {
            return BackendResult::failure(403);
        }
        std::error_code ec;
        if (!std::filesystem::exists(src, ec)) {
            return BackendResult::failure(404);
        }
        const bool existed = std::filesystem::exists(dst, ec);
        if (existed && !overwrite) {
            return BackendResult::failure(412);
        }
        std::filesystem::create_directories(dst.parent_path(), ec);
        const auto opts = std::filesystem::copy_options::recursive |
                          (overwrite ? std::filesystem::copy_options::overwrite_existing
                                     : std::filesystem::copy_options::none);
        std::filesystem::copy(src, dst, opts, ec);
        return ec ? BackendResult::failure(500, ec.message()) : BackendResult::success(existed ? 204 : 201);
    }

    BackendResult LocalWebDavBackend::move(std::string_view from, std::string_view to, bool overwrite)
    {
        const auto dst = resolve(to);
        std::error_code ec;
        const bool existed = std::filesystem::exists(dst, ec);
        if (existed && !overwrite) {
            return BackendResult::failure(412);
        }
        if (existed) {
            std::filesystem::remove_all(dst, ec);
        }
        auto copied = copy(from, to, true);
        if (!copied.ok) {
            return copied;
        }
        auto removed = remove(from);
        if (!removed.ok) {
            return removed;
        }
        return BackendResult::success(existed ? 204 : 201);
    }

    BackendResult LocalWebDavBackend::set_properties(std::string_view href, const std::vector<Property> &properties)
    {
        const auto path = resolve(href);
        if (!contains_root(path) || !std::filesystem::exists(path)) {
            return BackendResult::failure(404);
        }
        std::error_code ec;
        std::filesystem::create_directories(property_store_, ec);
        const auto store = property_store_ / (std::to_string(std::hash<std::string>{}(std::string(href))) + ".txt");
        std::ofstream out(store, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return BackendResult::failure(507);
        }
        for (const auto &p : properties) {
            out << p.name << '=' << p.value << '\n';
        }
        return BackendResult::success(207);
    }

    std::vector<Property> LocalWebDavBackend::dead_properties(std::string_view href) const
    {
        std::vector<Property> props;
        const auto store = property_store_ / (std::to_string(std::hash<std::string>{}(std::string(href))) + ".txt");
        std::ifstream in(store, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) {
                props.push_back(Property{ "DAV:", line.substr(0, eq), line.substr(eq + 1) });
            }
        }
        return props;
    }

    Quota LocalWebDavBackend::quota(std::string_view href) const
    {
        (void)href;
        Quota q;
        std::error_code ec;
        const auto sp = std::filesystem::space(root_, ec);
        if (!ec) {
            q.used_bytes = sp.capacity - sp.available;
            q.available_bytes = sp.available;
        }
        return q;
    }

    std::filesystem::path LocalWebDavBackend::resolve(std::string_view href) const
    {
        std::filesystem::path relative = path_from_utf8(clean_href(href)).lexically_normal();
        if (relative.is_absolute()) {
            relative = relative.relative_path();
        }
        return (root_ / relative).lexically_normal();
    }

    bool LocalWebDavBackend::contains_root(const std::filesystem::path &path) const
    {
#ifdef _WIN32
        const auto p = std::filesystem::absolute(path).lexically_normal().native();
        const auto r = root_.lexically_normal().native();
        auto lower = [](std::wstring s) {
            for (wchar_t &ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
            return s;
        };
        const auto pp = lower(p);
        const auto rr = lower(r);
        return pp == rr || pp.rfind(rr + L"\\", 0) == 0 || pp.rfind(rr + L"/", 0) == 0;
#else
        const auto p = std::filesystem::absolute(path).lexically_normal().string();
        const auto r = root_.lexically_normal().string();
        return p == r || p.rfind(r + "/", 0) == 0;
#endif
    }

    ResourceInfo LocalWebDavBackend::stat_path(const std::filesystem::path &path) const
    {
        ResourceInfo info;
        std::error_code ec;
        if (!contains_root(path) || !std::filesystem::exists(path, ec)) {
            return info;
        }
        info.exists = true;
        info.is_collection = std::filesystem::is_directory(path, ec);
        info.display_name = path_to_utf8(path.filename());
        info.content_type = info.is_collection ? "httpd/unix-directory" : extension_content_type(path);
        if (!info.is_collection) {
            info.content_length = std::filesystem::file_size(path, ec);
        }
        info.last_modified = file_time_to_sys(std::filesystem::last_write_time(path, ec));
        info.etag = "\"" + std::to_string(info.content_length) + "-" +
                    std::to_string(std::chrono::system_clock::to_time_t(info.last_modified)) + "\"";
        return info;
    }
}
