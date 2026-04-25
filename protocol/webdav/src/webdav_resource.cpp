#include "webdav_resource.h"
#include "webdav_xml.h"

#include <fstream>
#include <sstream>
#include <system_error>

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
            const auto ext = path.extension().string();
            if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
            if (ext == ".txt" || ext == ".log") return "text/plain; charset=utf-8";
            if (ext == ".json") return "application/json";
            if (ext == ".xml") return "application/xml";
            if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
            if (ext == ".png") return "image/png";
            if (ext == ".webp") return "image/webp";
            return "application/octet-stream";
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
            child.name = entry.path().filename().string();
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
        std::filesystem::path relative = std::filesystem::path(clean_href(href)).lexically_normal();
        if (relative.is_absolute()) {
            relative = relative.relative_path();
        }
        return (root_ / relative).lexically_normal();
    }

    bool LocalWebDavBackend::contains_root(const std::filesystem::path &path) const
    {
        const auto p = std::filesystem::absolute(path).lexically_normal().string();
        const auto r = root_.lexically_normal().string();
#ifdef _WIN32
        auto lower = [](std::string s) {
            for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        };
        const auto pp = lower(p);
        const auto rr = lower(r);
        return pp == rr || pp.rfind(rr + "\\", 0) == 0 || pp.rfind(rr + "/", 0) == 0;
#else
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
        info.display_name = path.filename().string();
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
