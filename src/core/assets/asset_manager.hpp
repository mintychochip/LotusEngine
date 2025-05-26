/**
**********************************************************************************************************************************************************************************************************************************
* @file:	asset_manager.hpp
* @author:	mintychochip
* @date:	2025年05月23日 18:41:27 Friday
* @brief:	
**********************************************************************************************************************************************************************************************************************************
**/

#pragma once

#include <filesystem>
#include <string>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
namespace fs_ = std::filesystem;

/*
    Detects changes of writes in files for hot-reloading of resources
*/

struct SharedBuffer
{
    std::vector<std::pair<std::string,fs_::file_time_type>> changed_keys;
    std::mutex changed_mutex;
};

class FileWatcher
{
    struct WatchedFile_
    {
        std::string path;
        fs_::file_time_type write_time;
    };

public:
    void watch(const std::string &key, const std::string &path)
    {
        if (!fs_::exists(path))
            return;
        auto time = fs_::last_write_time(path);
        std::unique_lock<std::shared_mutex> lock {files_mutex_};
        files_.insert({key,{path,time}});
    }

    void poll(SharedBuffer &buffer)
    {
        std::vector<std::pair<std::string,fs_::file_time_type>> local_changes;
        {
            std::shared_lock<std::shared_mutex> lock {files_mutex_};
            for (auto &[key,file] : files_) {
                if (!fs_::exists(file.path)) {
                    continue;
                }
                auto time = fs_::last_write_time(file.path);
                if (time != file.write_time) {
                    std::cout << key << ' ' << "changed" << std::flush << std::endl;
                    local_changes.push_back({key,time});
                }
            }
        }
        
        {
            std::unique_lock<std::shared_mutex> lock {files_mutex_};
            for (auto &[key,time] : local_changes) {
                auto it = files_.find(key);
                if (it != files_.end()) {
                    it->second.write_time = time;
                }
            }
        }

        {
            std::lock_guard lock{buffer.changed_mutex};
            buffer.changed_keys = std::move(local_changes);
        }
    }

private:
    std::unordered_map<std::string,WatchedFile_> files_;
    std::shared_mutex files_mutex_;
};

template <typename T>
class AssetManager
{
public:
    template <typename... Args>
    T *create(const std::string& key, const std::string& path, Args&&... args)
    {
        watcher_.watch(key, path);
        assets_.erase(key);
        auto [it,inserted] = assets_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(path, std::forward<Args>(args)...)
        );
        if (!inserted) {
            return nullptr;
        }
        return &it->second;
    }


    T *get(const std::string &key)
    {
        auto it = assets_.find(key);
        if (it == assets_.end())
            return nullptr;
        return &it->second;
    }

    void poll(SharedBuffer &buffer_)
    {
        std::lock_guard<std::mutex> lock {buffer_.changed_mutex};
        for (auto& changed_keys : buffer_.changed_keys) {

        }
    }

private:
    std::unordered_map<std::string, T> assets_;
};

template <typename T>
class AssetSystem
{
    public:
        void poll_watcher()
        {
            watcher_.poll(buffer_);
        }
    private:
        SharedBuffer buffer_;
        AssetManager<T> manager_;
        FileWatcher watcher_;
};
