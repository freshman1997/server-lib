//
// Created by jackeyuan on 2024/5/26.
//
#ifdef _WIN32
#include "base/utils/string_converter.h"

#include <windows.h>

namespace yuan::base::encoding
{
    using namespace yuan::std;

    std::string GBKToUTF8(const string &strGBK)
    {
        int n = MultiByteToWideChar(CP_ACP, 0, strGBK.c_str(),  - 1, NULL, 0);
        auto *str1 = new WCHAR[n];
        MultiByteToWideChar(CP_ACP, 0, strGBK.c_str(),  - 1, str1, n);
        n = WideCharToMultiByte(CP_UTF8, 0, str1, -1, NULL, 0, NULL, NULL);
        const auto str2 = new char[n];
        WideCharToMultiByte(CP_UTF8, 0, str1,  - 1, str2, n, NULL, NULL);
        string strOutUTF8 = str2;
        delete [] str1;
        delete [] str2;
        return strOutUTF8;
    }

    std::string UTF8ToGBK(const string &strUTF8){
        int len = MultiByteToWideChar(CP_UTF8, 0, strUTF8.c_str(), -1, NULL, 0);
        const auto wszGBK = new WCHAR[len + 1];
        memset(wszGBK, 0, (len+1)*sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)strUTF8.c_str(),  - 1, wszGBK, len);
        len = WideCharToMultiByte(CP_ACP, 0, wszGBK, -1, NULL, 0, NULL, NULL);
        const auto szGBK = new char[len + 1];
        memset(szGBK, 0, len + 1);
        WideCharToMultiByte(CP_ACP, 0, wszGBK,  - 1, szGBK, len, NULL, NULL);
        //strUTF8 = szGBK;
        string strTemp(szGBK);
        delete [] szGBK;
        delete [] wszGBK;
        return strTemp;
    }
}
#endif