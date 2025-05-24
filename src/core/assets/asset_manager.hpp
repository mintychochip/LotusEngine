#pragma once

#include <filesystem>
#include <string>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
namespace fs_ = std::filesystem;

class FileWatcher
{
    struct WatchedFile_
    {
        std::string path;
        fs_::file_time_type write_time;
    };

public:
    FileWatcher(std::function<void(const std::string&)> update_func) : callback_{std::move(update_func)} {}

    void watch(const std::string &key, const std::string &path)
    {
        if (!fs_::exists(path))
            return;
        auto time = fs_::last_write_time(path);
        std::unique_lock<std::shared_mutex> lock {mutex_};
        files_.insert({key,{path,time}});
    }

    void update()
    {
        std::vector<std::pair<std::string,fs_::file_time_type>> changed_keys;
        {
            std::shared_lock<std::shared_mutex> lock {mutex_};
            for (auto &[key,file] : files_) {
                if (!fs_::exists(file.path)) {
                    continue;
                }
                auto time = fs_::last_write_time(file.path);
                if (time != file.write_time) {
                    changed_keys.push_back({key,time});
                    callback_(key);
                }
            }
        }
        
        std::unique_lock<std::shared_mutex> lock {mutex_};
        for (auto &[key,time] : changed_keys) {
            auto it = files_.find(key);
            if (it != files_.end()) {
                it->second.write_time = time;
            }
        }
    }

private:
    std::function<void(const std::string &)> callback_;
    std::unordered_map<std::string,WatchedFile_> files_;
    std::shared_mutex mutex_;
};

template <typename T>
class AssetManager
{
public:
    AssetManager() : watcher_([this](const std::string& key) { on_change_reload(key); }) {}

    template <typename... Args>
    void create(const std::string &key, const std::string &path, Args &&...args)
    {
        T asset{path, std::forward<Args>(args)...};
        watcher_.watch(key,path);

        std::unique_lock<std::shared_mutex> lock{mutex_};
        assets_.emplace(key, std::move(asset));
    }

    void acquire(const std::string &key, const std::function<void(const T &)> fn)
    {
        std::shared_lock<std::shared_mutex> lock{mutex_};
        auto it = assets_.find(key);
        if (it != assets_.end())
            fn(it->second);
    }

    void poll()
    {
        watcher_.update();
    }

private:
    void on_change_reload(const std::string &key) 
    {
        std::unique_lock<std::shared_mutex> lock {mutex_};
        auto it = assets_.find(key);
        if (it != assets_.end()) {
            it->second.reload();
        }
    }
    std::shared_mutex mutex_;
    std::unordered_map<std::string, T> assets_;
    FileWatcher watcher_;
};
