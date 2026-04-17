#ifndef __YUAN_PLUGIN_LUA_HOST_BINDINGS_H__
#define __YUAN_PLUGIN_LUA_HOST_BINDINGS_H__

#include "plugin/plugin_context.h"

#include <mutex>

struct lua_State;

namespace yuan::plugin
{

    void lua_register_host_modules(lua_State * L, const PluginContext & context, size_t max_instructions_per_call, std::recursive_mutex * lua_mutex);

    void push_json_to_lua(lua_State * L, const nlohmann::json & j);

    void cleanup_lua_plugin_callbacks(lua_State * L, const std::string & plugin_name);

} // namespace yuan::plugin

#endif
