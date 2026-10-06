#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lotus/scene/streaming.hpp"

using namespace lotus;

namespace {

std::vector<StreamRegion> three_regions()
{
	return {
	        StreamRegion{"a", "a.json", {0.0f, 0.0f, 0.0f}, 10.0f},
	        StreamRegion{"b", "b.json", {100.0f, 0.0f, 0.0f}, 10.0f},
	        StreamRegion{"c", "c.json", {200.0f, 0.0f, 0.0f}, 10.0f},
	};
}

bool has(const std::vector<std::string>& v, const std::string& name)
{
	for (const std::string& s : v) {
		if (s == name) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST(ResidencyPolicyTest, LoadsOnlyRegionsTheViewerIsIn)
{
	const ResidencyPolicy policy;
	const std::vector<StreamRegion> regions = three_regions();

	// Viewer at the centre of region a: only a is in range.
	const auto load = policy.to_load(regions, {0.0f, 0.0f, 0.0f}, {});
	ASSERT_EQ(load.size(), 1u);
	EXPECT_EQ(load[0], "a");

	// Between a and b, outside both radii: nothing to load.
	EXPECT_TRUE(policy.to_load(regions, {50.0f, 0.0f, 0.0f}, {}).empty());

	// The load margin brings b in before the viewer reaches it, which is what
	// makes the load invisible rather than a pop.
	const auto ahead =
	        policy.to_load(regions, {50.0f, 0.0f, 0.0f}, {}, /*load_margin=*/45.0f);
	EXPECT_TRUE(has(ahead, "a"));
	EXPECT_TRUE(has(ahead, "b"));
}

TEST(ResidencyPolicyTest, NeverLoadsARegionThatIsAlreadyResident)
{
	const ResidencyPolicy policy;
	const std::vector<StreamRegion> regions = three_regions();

	// Loading a resident region again would duplicate its entities, so it must
	// not appear in the load set.
	const auto load =
	        policy.to_load(regions, {0.0f, 0.0f, 0.0f}, {"a", "b"});
	EXPECT_TRUE(load.empty());
}

TEST(ResidencyPolicyTest, UnloadsOnlyResidentRegionsOutOfRange)
{
	const ResidencyPolicy policy;
	const std::vector<StreamRegion> regions = three_regions();

	// Viewer in a, with b resident but far away: b unloads, a stays.
	const auto unload =
	        policy.to_unload(regions, {0.0f, 0.0f, 0.0f}, {"a", "b"}, 0.0f);
	ASSERT_EQ(unload.size(), 1u);
	EXPECT_EQ(unload[0], "b");

	// A region that is not resident is never reported, even if out of range:
	// unloading what was never loaded would be a no-op that still churns.
	EXPECT_TRUE(policy.to_unload(regions, {0.0f, 0.0f, 0.0f}, {}, 0.0f).empty());
}

TEST(ResidencyPolicyTest, HysteresisPreventsBoundaryThrashing)
{
	const ResidencyPolicy policy;
	const std::vector<StreamRegion> regions = three_regions();

	// Viewer just past a's edge (distance 11, radius 10).
	const glm::vec3 viewer{11.0f, 0.0f, 0.0f};

	// With no unload margin, a is unloaded the instant the viewer steps out.
	const auto eager = policy.to_unload(regions, viewer, {"a"}, 0.0f);
	EXPECT_TRUE(has(eager, "a"));

	// With a margin, a is kept until the viewer is well clear. The same viewer
	// position now keeps it, which is the whole point: no load/unload churn at
	// the boundary.
	const auto lazy = policy.to_unload(regions, viewer, {"a"}, 5.0f);
	EXPECT_TRUE(lazy.empty());
}

TEST(ResidencyPolicyTest, LoadAndUnloadAgreeAcrossAMove)
{
	// A walk from a to c must produce a monotone load/unload sequence with no
	// region both loaded and unloaded at the same step.
	const ResidencyPolicy policy;
	const std::vector<StreamRegion> regions = three_regions();
	std::vector<std::string> resident;

	for (float x = 0.0f; x <= 200.0f; x += 5.0f) {
		const glm::vec3 viewer{x, 0.0f, 0.0f};
		for (const std::string& name : policy.to_load(regions, viewer, resident, 5.0f)) {
			ASSERT_FALSE(has(resident, name)) << "loaded a resident region";
			resident.push_back(name);
		}
		for (const std::string& name :
		     policy.to_unload(regions, viewer, resident, 20.0f)) {
			ASSERT_TRUE(has(resident, name)) << "unloaded a non-resident region";
			resident.erase(std::remove(resident.begin(), resident.end(), name),
			               resident.end());
		}
	}

	// Arrived at c: c resident, a long gone.
	EXPECT_TRUE(has(resident, "c"));
	EXPECT_FALSE(has(resident, "a"));
}
