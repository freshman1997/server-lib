#include <unordered_map>
#include "net/http/content_type.h"

namespace net::http
{
    static const char * content_type_names[] = 
    {
        "text/plain",
        "text/html",
        "text/style",
        "text/javascript",
        "application/json",
        "multipart/form-data",
        "media/mp3",
        "media/ogg",
        "video/mp4",
        "video/flv",
    };

    static const std::unordered_map<std::string, content_type> content_type_mapping = 
    {
        {"text/plain", content_type::text_plain},
        {"text/html", content_type::text_html},
        {"text/style", content_type::text_style_sheet},
        {"text/javascript", content_type::text_javascript},
        {"application/json", content_type::application_json},
        {"multipart/form-data", content_type::multpart_form_data},
        {"media/mp3", content_type::media_mp3},
        {"media/ogg", content_type::media_ogg},
        {"video/mp4", content_type::video_mp4},
        {"video/flv", content_type::video_flv},
    };

    content_type find_content_type(const std::string &name)
    {
        auto it = content_type_mapping.find(name);
        return it == content_type_mapping.end() ? content_type::not_support : it->second;
    }
}