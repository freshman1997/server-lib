#ifndef __YUAN_API_H__
#define __YUAN_API_H__

#ifdef _WIN32
    #ifdef __cplusplus
        #define YUAN_API_C_IMPORT extern "C" __declspec(dllimport)
        #define YUAN_API_C_EXPORT extern "C" __declspec(dllexport)
    #else
        #define YUAN_API_C_IMPORT __declspec(dllimport)
        #define YUAN_API_C_EXPORT __declspec(dllexport)
    #endif
    #define YUAN_API_IMPORT __declspec(dllimport)
    #define YUAN_API_EXPORT __declspec(dllexport)
    #define YUAN_API __declspec(dllexport)
#else
    #ifdef __cplusplus
        #define YUAN_API_C_IMPORT extern "C"
        #define YUAN_API_C_EXPORT extern "C"
    #else
        #define YUAN_API_C_IMPORT
        #define YUAN_API_C_EXPORT
    #endif
    #define YUAN_API_IMPORT
    #define YUAN_API_EXPORT
    #define YUAN_API __attribute__((visibility("default")))
#endif

#endif // __YUAN_API_H__
