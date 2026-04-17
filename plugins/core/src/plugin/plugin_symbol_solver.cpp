#include "plugin/plugin_symbol_solver.h"
#include <cassert>
#include "base/utils/string_converter.h"

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace yuan::plugin
{
    void *PluginSymbolSolver::load_native_lib(const std::string & path)
    {
#if defined(__unix__) || defined(__APPLE__)
        void *handle = dlopen(path.c_str(), RTLD_LAZY);
        return handle;
#elif defined(_WIN32)
        const std::string &realPath = base::encoding::UTF8ToGBK(path.c_str());
        void *handle = LoadLibraryA(realPath.c_str());
        return handle;
#else
        return nullptr;
#endif
    }

    void PluginSymbolSolver::release_native_lib(void * handle)
    {
        if (!handle) {
            return;
        }

#if defined(__unix__) || defined(__APPLE__)
        dlclose(handle);
#elif defined(_WIN32)
        FreeLibrary((HMODULE)handle);
#else
        (void)handle;
#endif
    }

    void *PluginSymbolSolver::find_symbol(void * handle, const std::string & symbolName)
    {
#if defined(__unix__) || defined(__APPLE__)
        void *addr = dlsym(handle, symbolName.c_str());
        return addr;
#elif defined(_WIN32)
        return reinterpret_cast<void *>(GetProcAddress((HMODULE)handle, symbolName.c_str()));
#else
        (void)handle;
        (void)symbolName;
        return nullptr;
#endif
    }
}