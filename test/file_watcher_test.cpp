#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "lotus/io/file_watcher.hpp"

using namespace lotus;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string& name)
{
	return fs::temp_directory_path() / name;
}

void write_file(const fs::path& path, const std::string& text)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

void touch_newer(const fs::path& path)
{
	const auto now = fs::last_write_time(path);
	fs::last_write_time(path, now + std::chrono::seconds(5));
}

} // namespace

TEST(FileWatcherTest, FirstPollIsBaselineThenChangeIsReportedOnce)
{
	const fs::path path = temp_path("lotus_watcher_test.txt");
	write_file(path, "a");

	FileWatcher watcher;
	watcher.watch("k", path);

	EXPECT_TRUE(watcher.poll().empty()); // baseline only
	EXPECT_TRUE(watcher.poll().empty()); // unchanged

	touch_newer(path);
	const auto changed = watcher.poll();
	ASSERT_EQ(changed.size(), 1u);
	EXPECT_EQ(changed[0], "k");

	EXPECT_TRUE(watcher.poll().empty()); // consumed

	fs::remove(path);
}

TEST(FileWatcherTest, MissingFileIsNotReported)
{
	const fs::path path = temp_path("lotus_watcher_missing.txt");
	fs::remove(path);

	FileWatcher watcher;
	watcher.watch("k", path);
	EXPECT_TRUE(watcher.poll().empty());

	// Appearing later is a change (baseline was never established).
	write_file(path, "hello");
	const auto changed = watcher.poll();
	ASSERT_EQ(changed.size(), 1u);

	fs::remove(path);
}

TEST(FileWatcherTest, UnwatchStopsReporting)
{
	const fs::path path = temp_path("lotus_watcher_unwatch.txt");
	write_file(path, "a");

	FileWatcher watcher;
	watcher.watch("k", path);
	watcher.poll();

	watcher.unwatch("k");
	EXPECT_FALSE(watcher.watching("k"));
	touch_newer(path);
	EXPECT_TRUE(watcher.poll().empty());

	fs::remove(path);
}

TEST(FileWatcherTest, TracksMultipleKeysIndependently)
{
	const fs::path a = temp_path("lotus_watcher_a.txt");
	const fs::path b = temp_path("lotus_watcher_b.txt");
	write_file(a, "a");
	write_file(b, "b");

	FileWatcher watcher;
	watcher.watch("a", a);
	watcher.watch("b", b);
	watcher.poll();

	touch_newer(b);
	const auto changed = watcher.poll();
	ASSERT_EQ(changed.size(), 1u);
	EXPECT_EQ(changed[0], "b");

	fs::remove(a);
	fs::remove(b);
}
