#ifndef __PLUGIN_SYMBOL_SOLVER_H__
#define __PLUGIN_SYMBOL_SOLVER_H__
#include <string>

namespace yuan::plugin 
{
    class PluginSymbolSolver
    {
    public:
        static void * load_native_lib(const std::string &path);

        static void release_native_lib(void *handle);

        static void * find_symbol(void *handle, const std::string &symbolName);
    };
}

#endif // __PLUGIN_SYMBOL_SOLVER_H__
