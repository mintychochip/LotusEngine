#include "lotus/io/file_watcher.hpp"

namespace lotus {
namespace {

bool read_mtime(const std::filesystem::path& path,
                std::filesystem::file_time_type& out)
{
	std::error_code error;
	if (!std::filesystem::exists(path, error) || error) {
		return false;
	}
	out = std::filesystem::last_write_time(path, error);
	return !error;
}

} // namespace

void FileWatcher::watch(std::string key, const std::filesystem::path& path)
{
	Entry& entry = entries_[std::move(key)];
	entry.path   = path;
	// Baseline now if the file already exists, so watching never reports a
	// spurious change. If it does not exist yet, its first appearance counts
	// as a change.
	entry.has_baseline = read_mtime(path, entry.mtime);
}

void FileWatcher::unwatch(const std::string& key)
{
	entries_.erase(key);
}

void FileWatcher::clear()
{
	entries_.clear();
}

std::vector<std::string> FileWatcher::poll()
{
	std::vector<std::string> changed;
	for (auto& [key, entry] : entries_) {
		std::filesystem::file_time_type mtime;
		if (!read_mtime(entry.path, mtime)) {
			// Missing right now (e.g. mid-rename write): keep the baseline
			// and wait rather than reporting a spurious change.
			continue;
		}
		if (!entry.has_baseline) {
			entry.mtime        = mtime;
			entry.has_baseline = true;
			changed.push_back(key);
			continue;
		}
		if (mtime != entry.mtime) {
			entry.mtime = mtime;
			changed.push_back(key);
		}
	}
	return changed;
}

bool FileWatcher::watching(const std::string& key) const
{
	return entries_.find(key) != entries_.end();
}

} // namespace lotus
