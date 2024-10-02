#include "client/command_scanner.h"
#include <iostream>
#include <string>

namespace net::ftp 
{
    std::string CommandScanner::simpleCommand()
    {
        std::string res;
        while (res.empty())
        {
            std::cout << "please enter command >> ";
            std::getline(std::cin, res);
        }
        return res + "\r\n";
    }
}