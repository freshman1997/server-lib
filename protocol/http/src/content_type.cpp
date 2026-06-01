#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>
#include "content_type.h"

namespace yuan::net::http
{
    static const char *ContentType_names[] =
        {
            "text/plain",
            "text/html",
            "text/css",
            "text/javascript",
            "application/json",
            "multipart/form-data",
            "multipart/byteranges",
            "application/x-www-form-urlencoded",
            "audio/mpeg",
            "audio/ogg",
            "video/mp4",
            "video/flv",
            "video/mkv",
        };

    static const std::unordered_map<std::string, std::pair<std::string, ContentType> > ContentType_mapping =
        {
            { "text/plain", { ".txt", ContentType::text_plain } },
            { "text/html", { ".html", ContentType::text_html } },
            { "text/css", { ".css", ContentType::text_style_sheet } },
            { "text/javascript", { ".js", ContentType::text_javascript } },
            { "application/json", { ".json", ContentType::application_json } },
            { "multipart/form-data", { "", ContentType::multpart_form_data } },
            { "multipart/byteranges", { "", ContentType::multpart_byte_ranges } },
            { "application/x-www-form-urlencoded", { "", ContentType::x_www_form_urlencoded } },
            { "audio/mpeg", { ".mp3", ContentType::audio_mpeg } },
            { "audio/ogg", { ".ogg", ContentType::audio_ogg } },
            { "video/mp4", { ".mp4", ContentType::video_mp4 } },
            { "video/flv", { ".flv", ContentType::video_flv } },
            { "video/mkv", { ".mkv", ContentType::video_mkv } },
        };

    ContentType find_content_type(const std::string & name)
    {
        auto it = ContentType_mapping.find(name);
        return it == ContentType_mapping.end() ? ContentType::not_support : it->second.second;
    }

    std::string get_content_type(const std::string & ext)
    {
        std::string normalized = ext;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        for (const auto &item : ContentType_mapping) {
            if (!item.first.empty() && item.second.first == normalized) {
                return item.first;
            }
        }

        static const std::unordered_map<std::string, std::string> common_types = {
            { ".htm", "text/html" },
            { ".mjs", "text/javascript" },
            { ".xml", "application/xml" },
            { ".svg", "image/svg+xml" },
            { ".wasm", "application/wasm" },
            { ".mp4", "video/mp4" },
            { ".m4v", "video/x-m4v" },
            { ".webm", "video/webm" },
            { ".mov", "video/quicktime" },
            { ".avi", "video/x-msvideo" },
            { ".mkv", "video/x-matroska" },
            { ".flv", "video/x-flv" },
            { ".ts", "video/mp2t" },
            { ".m3u8", "application/vnd.apple.mpegurl" },
            { ".mpd", "application/dash+xml" },
            { ".mp3", "audio/mpeg" },
            { ".m4a", "audio/mp4" },
            { ".aac", "audio/aac" },
            { ".wav", "audio/wav" },
            { ".oga", "audio/ogg" },
            { ".ogg", "audio/ogg" },
            { ".opus", "audio/opus" },
            { ".flac", "audio/flac" },
            { ".png", "image/png" },
            { ".jpg", "image/jpeg" },
            { ".jpeg", "image/jpeg" },
            { ".gif", "image/gif" },
            { ".webp", "image/webp" },
            { ".ico", "image/x-icon" },
            { ".pdf", "application/pdf" },
            { ".zip", "application/zip" },
            { ".gz", "application/gzip" },
            { ".br", "application/octet-stream" },
            { ".map", "application/json" },
            { ".woff", "font/woff" },
            { ".woff2", "font/woff2" },
            { ".ttf", "font/ttf" },
            { ".otf", "font/otf" },
            { ".eot", "application/vnd.ms-fontobject" },
        };

        auto it = common_types.find(normalized);
        if (it != common_types.end()) {
            return it->second;
        }

        return "application/octet-stream";
    }
}
