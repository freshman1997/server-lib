#include <cstdio>
#include <iostream>
#include "net/http/content/types.h"

namespace net::http 
{
    FormDataContent::~FormDataContent()
    {
        for (const auto &item : properties) {
            if (item.second.first) {
                std::cout << "trying remove tmp file " << item.second.second << std::endl;
                if (!std::remove(item.second.second.c_str())) {
                    std::cout << "remove tmp file successfully!\n";
                } else {
                    std::cout << "remove tmp file failed!!\n";
                }
                std::cout.flush();
            }
        }
    }
}