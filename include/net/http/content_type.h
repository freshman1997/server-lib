#ifndef __NET__HTTP_CONTENT_TYPE__
#define __NET__HTTP_CONTENT_TYPE__
#include <string>

namespace net::http
{
    enum class content_type : char
    {
        not_support = -1,
        text_plain = 0,
        text_html,
        text_style_sheet,
        text_javascript,
        application_json,

        multpart_form_data,
        x_www_form_urlencoded,

        media_mp3,
        media_ogg,
        video_mp4,
        video_flv,
        MAX
    };

    content_type find_content_type(const std::string &name);
}

#endif