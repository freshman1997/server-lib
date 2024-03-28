#include "net/http/ops/option.h"

#define KEY_TO_STRING(key) (#key)

namespace net::http::config
{
    const char * config_file_name = "http.json";

    const char * server_name = KEY_TO_STRING(server_name);

    const char * max_content_length = KEY_TO_STRING(max_content_length);

    const char * parse_form_data_content_types = KEY_TO_STRING(parse_form_data_content_types);

    const char * static_file_paths = KEY_TO_STRING(static_file_paths);
    const char * static_file_paths_root = "root";
    const char * static_file_paths_path = "path";

    const char * playable_types = KEY_TO_STRING(playable_types);

    // 最大包体长度默认 2 m
    const uint32_t client_max_content_length = 1024 * 1024 * 2;

    const bool form_data_upload_save = true;
}
