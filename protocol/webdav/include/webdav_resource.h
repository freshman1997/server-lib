#ifndef __NET_WEBDAV_RESOURCE_H__
#define __NET_WEBDAV_RESOURCE_H__

#include "webdav_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::net::webdav
{
    struct ResourceInfo
    {
        bool exists = false;
        bool is_collection = false;
        std::uint64_t content_length = 0;
        std::chrono::system_clock::time_point last_modified{};
        std::string etag;
        std::string content_type = "application/octet-stream";
        std::string display_name;
    };

    struct ChildResource
    {
        std::string name;
        ResourceInfo info;
    };

    struct BackendResult
    {
        bool ok = false;
        int status = 500;
        std::string message;

        static BackendResult success(int code = 200) { return { true, code, {} }; }
        static BackendResult failure(int code, std::string msg = {}) { return { false, code, std::move(msg) }; }
    };

    class WebDavResourceBackend
    {
    public:
        virtual ~WebDavResourceBackend() = default;

        virtual ResourceInfo stat(std::string_view href) const = 0;
        virtual std::vector<ChildResource> list(std::string_view href) const = 0;
        virtual std::optional<std::string> read(std::string_view href) const = 0;
        virtual std::optional<std::filesystem::path> file_path_for_read(std::string_view href) const;
        virtual BackendResult write(std::string_view href, std::string_view body, bool create_parents) = 0;
        virtual BackendResult write_from_file(std::string_view href,
                                              const std::filesystem::path &source,
                                              bool create_parents);
        virtual BackendResult make_collection(std::string_view href) = 0;
        virtual BackendResult remove(std::string_view href) = 0;
        virtual BackendResult copy(std::string_view from, std::string_view to, bool overwrite) = 0;
        virtual BackendResult move(std::string_view from, std::string_view to, bool overwrite) = 0;
        virtual BackendResult set_properties(std::string_view href, const std::vector<Property> &properties) = 0;
        virtual std::vector<Property> dead_properties(std::string_view href) const = 0;
        virtual Quota quota(std::string_view href) const = 0;
    };

    class LocalWebDavBackend : public WebDavResourceBackend
    {
    public:
        explicit LocalWebDavBackend(std::filesystem::path root);

        ResourceInfo stat(std::string_view href) const override;
        std::vector<ChildResource> list(std::string_view href) const override;
        std::optional<std::string> read(std::string_view href) const override;
        std::optional<std::filesystem::path> file_path_for_read(std::string_view href) const override;
        BackendResult write(std::string_view href, std::string_view body, bool create_parents) override;
        BackendResult write_from_file(std::string_view href,
                                      const std::filesystem::path &source,
                                      bool create_parents) override;
        BackendResult make_collection(std::string_view href) override;
        BackendResult remove(std::string_view href) override;
        BackendResult copy(std::string_view from, std::string_view to, bool overwrite) override;
        BackendResult move(std::string_view from, std::string_view to, bool overwrite) override;
        BackendResult set_properties(std::string_view href, const std::vector<Property> &properties) override;
        std::vector<Property> dead_properties(std::string_view href) const override;
        Quota quota(std::string_view href) const override;

    private:
        std::filesystem::path resolve(std::string_view href) const;
        bool contains_root(const std::filesystem::path &path) const;
        ResourceInfo stat_path(const std::filesystem::path &path) const;

        std::filesystem::path root_;
        std::filesystem::path property_store_;
    };
}

#endif
