#include "url.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    std::unordered_map<std::string, std::vector<std::string>> params;
    if (!require(yuan::url::decode_parameters(std::string_view("/game/create_role?player_uid=42&name=SmokeRole&space=hello+world"), params),
                 "query parameters should decode")) {
        return EXIT_FAILURE;
    }
    if (!require(params["player_uid"].size() == 1 && params["player_uid"].front() == "42",
                 "first query parameter should decode as its own key")) {
        return EXIT_FAILURE;
    }
    if (!require(params["name"].size() == 1 && params["name"].front() == "SmokeRole",
                 "second query parameter should decode as its own key")) {
        return EXIT_FAILURE;
    }
    if (!require(params["space"].size() == 1 && params["space"].front() == "hello world",
                 "query plus should decode as space")) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
