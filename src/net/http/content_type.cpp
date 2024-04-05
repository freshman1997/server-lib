#include <unordered_map>
#include <utility>
#include "net/http/content_type.h"

namespace net::http
{
    static const char * ContentType_names[] = 
    {
        "text/plain",
        "text/html",
        "text/style",
        "text/javascript",
        "application/json",
        "multipart/form-data",
        "application/x-www-form-urlencoded",
        "media/mp3",
        "media/ogg",
        "video/mp4",
        "video/flv",
        "video/mkv",
    };

    static const std::unordered_map<std::string, std::pair<std::string, ContentType>> ContentType_mapping = 
    {
        {"text/plain", {".txt", ContentType::text_plain} },
        {"text/html", {".html", ContentType::text_html}},
        {"text/style", {".css", ContentType::text_style_sheet}},
        {"text/javascript", {".js", ContentType::text_javascript}},
        {"application/json", {".json", ContentType::application_json}},
        {"multipart/form-data", {"", ContentType::multpart_form_data}},
        {"application/x-www-form-urlencoded", {"", ContentType::x_www_form_urlencoded}},
        {"media/mp3", {".mp3", ContentType::media_mp3}},
        {"media/ogg", {".ogg", ContentType::media_ogg}},
        {"video/mp4", {".mp4", ContentType::video_mp4}},
        {"video/flv", {".flv", ContentType::video_flv}},
        {"video/mkv", {".mkv", ContentType::video_mkv}},
    };

    ContentType find_content_type(const std::string &name)
    {
        auto it = ContentType_mapping.find(name);
        return it == ContentType_mapping.end() ? ContentType::not_support : it->second.second;
    }

    std::string get_content_type(const std::string &ext)
    {
        for (const auto &item : ContentType_mapping) {
            if (!item.first.empty() && item.second.first == ext) {
                return item.first;
            }
        }

        return {};
    }
}