#ifndef __YUAN_API_H__
#define __YUAN_API_H__

#ifdef WIN32
    #ifdef __cplusplus
        #define YUAN_API_C_IMPORT extern "C" __declspec(dllimport)
        #define YUAN_API_C_EXPORT extern "C" __declspec(dllexport)
    #endif
    #define YUAN_API_IMPORT extern __declspec(dllimport)
    #define YUAN_API_EXPORT extern __declspec(dllexport)
#else
    #ifdef __cplusplus
        #define YUAN_API_C_IMPORT extern "C"
        #define YUAN_API_C_EXPORT extern "C"
    #endif
    #define YUAN_API_IMPORT extern
    #define YUAN_API_EXPORT extern
    #define YUAN_API __attribute__((visibility("default")))
#endif

#endif // __YUAN_API_H__
