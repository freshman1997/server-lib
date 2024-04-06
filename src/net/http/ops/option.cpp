#include "net/http/ops/option.h"
#include "net/http/ops/config_manager.h"
#include "singleton/singleton.h"

#define KEY_TO_STRING(key) (#key)

namespace net::http::config
{
    int connection_idle_timeout = 30 * 1000;
    
    const char * config_file_name = "http.json";

    const char * server_name = KEY_TO_STRING(server_name);

    const char * parse_form_data_content_types = KEY_TO_STRING(parse_form_data_content_types);

    const char * static_file_paths = KEY_TO_STRING(static_file_paths);
    const char * static_file_paths_root = "root";
    const char * static_file_paths_path = "path";

    const char * playable_types = KEY_TO_STRING(playable_types);

    // 最大包体长度默认 2 m
    uint32_t client_max_content_length = 1024 * 1024 * 2;

    bool close_idle_connection = false;

    bool form_data_upload_save = false;

    void load_config()
    {
        auto &cfgManager = singleton::Singleton<HttpConfigManager>();

        connection_idle_timeout = cfgManager.get_bool_property(KEY_TO_STRING(max_content_length), connection_idle_timeout);
        client_max_content_length = cfgManager.get_bool_property(KEY_TO_STRING(max_content_length), client_max_content_length);
        close_idle_connection = cfgManager.get_bool_property(KEY_TO_STRING(close_idle_connection), close_idle_connection);
        form_data_upload_save = cfgManager.get_bool_property(KEY_TO_STRING(form_data_upload_save), form_data_upload_save);
    }
}
