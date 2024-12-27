#include "plugin/plugin_symbol_solver.h"
#include <cassert>

#ifdef unix
#include <dlfcn.h>
#elif WIN32
#elif __APPLE__
#endif

namespace yuan::plugin 
{
    void * PluginSymbolSolver::load_native_lib(const std::string &path)
    {
    #ifdef unix
        void *handle = dlopen(path.c_str(), RTLD_LAZY);
        return handle;
    #else
        return nullptr;
    #endif
    }

    void PluginSymbolSolver::release_native_lib(void *handle)
    {
        assert(handle);
    #ifdef unix
        dlclose(handle);
    #else
        assert(false);
    #endif
    }

    void * PluginSymbolSolver::find_symbol(void *handle, const std::string &symbolName)
    {
    #ifdef unix
        void *addr = dlsym(handle, symbolName.c_str());
        return addr;
    #else
        return nullptr;
    #endif
    }
}