#include "net/ftp/client/command_scanner.h"
#include <iostream>

namespace net::ftp 
{
    std::string CommandScanner::simpleCommand()
    {
        std::string res;
        while (res.empty())
        {
            std::cout << "please enter command >> ";
            std::cin >> res;
        }
        return res;
    }
}