#include "plugin/plugin_symbol_solver.h"
#include <cassert>
#include "base/utils/string_converter.h"

#ifdef unix
#include <dlfcn.h>
#elif _WIN32
#include <windows.h>
#elif __APPLE__
#endif

namespace yuan::plugin 
{
    void * PluginSymbolSolver::load_native_lib(const std::string &path)
    {
    #ifdef unix
        void *handle = dlopen(path.c_str(), RTLD_LAZY);
        return handle;
    #elif _WIN32
        const std::string &realPath = base::encoding::UTF8ToGBK(path.c_str());
        void *handle = LoadLibraryA(realPath.c_str());
        return handle;
    #else
        return nullptr;
    #endif
    }

    void PluginSymbolSolver::release_native_lib(void *handle)
    {
        if (!handle) {
            return;
        }
        
    #ifdef unix
        dlclose(handle);
    #elif _WIN32
        FreeLibrary((HMODULE)handle);
    #else
        assert(false);
    #endif
    }

    void * PluginSymbolSolver::find_symbol(void *handle, const std::string &symbolName)
    {
    #ifdef unix
        void *addr = dlsym(handle, symbolName.c_str());
        return addr;
    #elif _WIN32
        return reinterpret_cast<void *>(GetProcAddress((HMODULE)handle, symbolName.c_str()));
    #else
        return nullptr;
    #endif
    }
}