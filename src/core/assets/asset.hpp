#pragma once

#include <filesystem>
#include <string>

class Asset
{
public:
    virtual void reload() = 0;
    const std::string &path() const
    {
        return path_;
    }
protected:
    Asset(const std::string& path) : path_{path} {}
    const std::string path_;
    std::filesystem::file_time_type last_write;
};