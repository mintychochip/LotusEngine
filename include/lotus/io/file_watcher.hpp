#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lotus {

/// Poll-based file watcher. Compares last-write times instead of using
/// OS-specific notification APIs, so it behaves identically everywhere
/// (including inside the editor's frame loop).
///
/// A file that is briefly missing (editors often write via rename) is not
/// reported; the watcher keeps waiting for it to reappear.
class FileWatcher {
public:
	/// Starts watching \p path under \p key. If the file already exists its
	/// current time is taken as the baseline, so watching does not report a
	/// change immediately. A file that does not exist yet reports its first
	/// appearance as a change.
	void watch(std::string key, const std::filesystem::path& path);
	void unwatch(const std::string& key);
	void clear();

	/// Keys of files modified since the previous poll.
	std::vector<std::string> poll();

	bool watching(const std::string& key) const;

private:
	struct Entry {
		std::filesystem::path path;
		std::filesystem::file_time_type mtime{};
		bool has_baseline{false};
	};
	std::map<std::string, Entry> entries_;
};

} // namespace lotus
