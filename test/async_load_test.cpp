#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include "lotus/scene/async_load.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

using namespace lotus;

namespace {

/// Writes a scene document with \p count entities and returns its path.
std::string write_scene(const std::string& path, int count)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << R"({"version":3,"name":"async_src","entities":[)";
	for (int i = 0; i < count; ++i) {
		if (i > 0) {
			out << ',';
		}
		out << R"({"name":"e)" << i
		    << R"(","components":{"Transform":{"position":[0.0,0.0,0.0]}}})";
	}
	out << "]}";
	return path;
}

/// Pumps poll() until it settles or the deadline passes. Returns the final
/// state. Bounded so a hang fails the test rather than blocking the suite.
AsyncSceneLoad::State pump(AsyncSceneLoad& load, int timeout_ms = 5000)
{
	const auto deadline =
	        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		const AsyncSceneLoad::State state = load.poll();
		if (state == AsyncSceneLoad::State::Ready ||
		    state == AsyncSceneLoad::State::Failed) {
			return state;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return AsyncSceneLoad::State::Loading;
}

} // namespace

TEST(AsyncSceneLoadTest, StartsIdleAndPollIsNonBlocking)
{
	AsyncSceneLoad load;
	EXPECT_EQ(load.state(), AsyncSceneLoad::State::Idle);
	// Polling with nothing started must not change state or block.
	EXPECT_EQ(load.poll(), AsyncSceneLoad::State::Idle);
	EXPECT_EQ(load.poll(), AsyncSceneLoad::State::Idle);
}

TEST(AsyncSceneLoadTest, LoadsASceneAndTakesItOnce)
{
	const std::string path = write_scene("async_load_test.json", 12);

	AsyncSceneLoad load;
	ASSERT_TRUE(load.start(path));
	ASSERT_EQ(pump(load), AsyncSceneLoad::State::Ready);
	EXPECT_EQ(load.state(), AsyncSceneLoad::State::Ready);

	Scene target{"live"};
	target.create("pre_existing");
	ASSERT_TRUE(load.take(target));

	// The loaded document replaced the target wholesale, and the scene is
	// usable: GUIDs resolved, entities addressable.
	EXPECT_EQ(target.registry().view<Name>().size(), 12u);
	for (const Entity e : target.registry().view<Name>()) {
		EXPECT_NE(target.guid_of(e).value, 0u);
	}

	// Consumed exactly once.
	EXPECT_EQ(load.state(), AsyncSceneLoad::State::Idle);
	Scene other{"other"};
	EXPECT_FALSE(load.take(other));

	std::remove(path.c_str());
}

TEST(AsyncSceneLoadTest, MissingFileFailsWithoutTouchingTheTarget)
{
	AsyncSceneLoad load;
	ASSERT_TRUE(load.start("definitely_missing_async_scene.json"));
	EXPECT_EQ(pump(load), AsyncSceneLoad::State::Failed);
	EXPECT_FALSE(load.error().empty());

	Scene target{"live"};
	const Entity survivor = target.create("survivor");
	EXPECT_FALSE(load.take(target));
	EXPECT_NE(target.find("survivor"), NullEntity);
	EXPECT_EQ(target.registry().view<Name>().size(), 1u);
	EXPECT_EQ(target.guid_of(survivor).value, 1u);
}

TEST(AsyncSceneLoadTest, MalformedFileFailsAndReportsWhy)
{
	const std::string path = "async_bad_test.json";
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << R"({"version":3,"name":"bad","entities":42})";
	}

	AsyncSceneLoad load;
	ASSERT_TRUE(load.start(path));
	EXPECT_EQ(pump(load), AsyncSceneLoad::State::Failed);
	EXPECT_FALSE(load.error().empty());

	std::remove(path.c_str());
}

TEST(AsyncSceneLoadTest, RejectsASecondStartWhileLoading)
{
	const std::string path = write_scene("async_second_test.json", 2000);

	AsyncSceneLoad load;
	ASSERT_TRUE(load.start(path));
	// Whether the first finished already is irrelevant: a second start while
	// Loading must be refused, and after settling it must be accepted.
	if (load.state() == AsyncSceneLoad::State::Loading) {
		EXPECT_FALSE(load.start(path));
	}
	ASSERT_NE(pump(load), AsyncSceneLoad::State::Loading);
	EXPECT_TRUE(load.start(path));
	ASSERT_NE(pump(load), AsyncSceneLoad::State::Loading);

	std::remove(path.c_str());
}

TEST(AsyncSceneLoadTest, PollNeverBlocksTheCallingThread)
{
	// The property that matters: a poll returns promptly even while the worker
	// is still running, so a frame loop cannot stall on it. This times ONE poll
	// taken immediately after start, on a file big enough that the parse cannot
	// have finished yet. A blocking implementation waits for the parse and
	// returns Ready here, failing both assertions.
	const std::string path = write_scene("async_poll_test.json", 60000);

	AsyncSceneLoad load;
	ASSERT_TRUE(load.start(path));
	ASSERT_EQ(load.state(), AsyncSceneLoad::State::Loading);

	const auto start = std::chrono::steady_clock::now();
	const AsyncSceneLoad::State first = load.poll();
	const double elapsed_ms = std::chrono::duration<double, std::milli>(
	                                  std::chrono::steady_clock::now() - start)
	                                  .count();

	// Either the worker had not finished (Loading, the expected case on a file
	// this size) or it had. Both are fine for correctness -- but the poll must
	// have returned quickly either way, which is the property under test.
	EXPECT_LT(elapsed_ms, 50.0)
	    << "a single poll took " << elapsed_ms << " ms, so it waited";
	EXPECT_EQ(first, AsyncSceneLoad::State::Loading)
	    << "the poll waited for the worker instead of reporting progress";

	ASSERT_NE(pump(load), AsyncSceneLoad::State::Loading);
	std::remove(path.c_str());
}
