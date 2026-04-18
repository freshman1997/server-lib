#ifndef __QUICKJS_LIB_WRAPPER_H__
#define __QUICKJS_LIB_WRAPPER_H__

#include <cassert>

#ifndef CONFIG_VERSION
#define CONFIG_VERSION "2025-09-13"
#endif

extern "C" {
#include "quickjs.h"
#include "quickjs-libc.h"
}

#ifdef __cplusplus
static inline JSCFunctionListEntry js_cfunc_def_entry(const char *name, int length, JSCFunction *func1)
{
    JSCFunctionListEntry entry{};
    entry.name = name;
    entry.prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
    entry.def_type = JS_DEF_CFUNC;
    entry.magic = 0;
    entry.u.func.length = static_cast<uint8_t>(length);
    entry.u.func.cproto = JS_CFUNC_generic;
    entry.u.func.cfunc.generic = func1;
    return entry;
}

#undef JS_CFUNC_DEF
#define JS_CFUNC_DEF(name, length, func1) js_cfunc_def_entry(name, length, func1)
#endif

#endif // __QUICKJS_LIB_WRAPPER_H__
