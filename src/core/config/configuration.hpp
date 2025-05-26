#pragma once

#include <string>

class Configuration
{
    virtual std::string get_string(const std::string &key) = 0;
    std::string get_string(const std::string &key, const std::string &def)
    {
        std::string res = get_string(key);
        return res.empty() ? def : res;
    }
};