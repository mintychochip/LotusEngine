# Hi-Z Occlusion Culling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cull entities hidden behind other geometry using a GPU-built hierarchical depth buffer, without ever blocking the render loop or culling something that is visible.

**Architecture:** The scene pass writes linear view depth to a second color target. A compute shader reduces that into a min-filtered mip chain (Hi-Z). Mips `2..N` are downloaded asynchronously and tested on the CPU against each entity's bounding sphere. The test is a pure C++ class with no SDL dependency, so it is unit-testable and keeps `Scene::build_vertices` testable.

**Tech Stack:** C++20, SDL3 GPU (Vulkan backend), GLSL 450 → SPIR-V via `glslangValidator`, GLM (zero-to-one depth), GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-05-hiz-occlusion-culling-design.md` (commit `e50b6a4`)

## Global Constraints

These apply to every task. Copy them verbatim.

- Occlusion never blocks. A frame whose fence is not ready skips occlusion and reports `occlusion_ready() == false`.
- Occlusion never throws. A pipeline or format failure disables the feature and logs once; the engine keeps running.
- The test is one-directional. Culling something visible is a bug. Drawing something hidden is a cost, not a bug.
- An empty, missing, or resized pyramid treats every entity as visible.
- `Scene::build_vertices` keeps its behaviour and signature when passed no pyramid. Existing tests compile and pass unchanged.
- Occlusion is derived per frame. It is never serialized.
- The depth target is cleared to a far value every frame. Never rely on unwritten texels; `min` reduction turns an unwritten texel into a scene-wide false occlusion.
- `init_offscreen` creates and sizes the pyramid texture exactly as the windowed path does.
- `kDepthFar` is `1.0e9f` in C++ and `1.0e9` in `depth.comp`. These two must stay equal.
- No CMake changes. `CMakeLists.txt` globs `src/*.cpp` (line 49) and `test/*.cpp` (line 97) with `CONFIGURE_DEPENDS`.
- Build: `cd build && cmake --build . -j"$(nproc)"`. Test binary: `build/bin/lotus_tests`. Baseline is **105 passing tests**; never leave it lower.
- Shader compiler: `GLSLANG=/tmp/shadertools/usr/bin/glslangValidator ./shaders/build-shaders.sh`. If that path is gone, re-extract per the spec's Build section.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/lotus/scene/depth_pyramid.hpp` | `DepthPyramid`: CPU-side mip chain, the `kDepthFar` constant, the `occluded` query. Includes **no** SDL header. |
| `src/scene/depth_pyramid.cpp` | Min-reduction across mips, projection, and the occlusion test. |
| `test/occlusion_test.cpp` | All CPU tests: reduction, query, and scene integration. |
| `shaders/depth.comp` | One mip level of min-reduction, run once per level. |
| `shaders/lit.vert` / `shaders/lit.frag` | Gain `view` matrix and a linear-depth output. |
| `shaders/build-shaders.sh` | Extended from `vert frag` to `vert frag comp`. |
| `include/lotus/gpu/renderer.hpp` / `src/gpu/renderer.cpp` | Pyramid texture, compute pipeline, transfer buffer, fence, and the CPU copy. |
| `include/lotus/scene/scene.hpp` / `src/scene/scene.cpp` | Thread the pyramid into culling; add `occluded_entities()`. |
| `include/lotus/scene/components.hpp` | `Camera3D::view()` and `Camera2D::view()`, needed for linear depth. |
| `app/main.cpp` | A wall in front of the LOD sphere so occlusion is visible. |
| `src/ui/editor.cpp` | Occluded count on the existing stats line. |
| `test/occlusion_gpu_test.cpp` | Headless GPU gate driving `init_offscreen`; asserts the pyramid reaches the CPU. |

Generated, committed: `include/lotus/gpu/depth.comp.spv.h`, plus regenerated `lit.vert.spv.h` and `lit.frag.spv.h`.

---

### Task 1: DepthPyramid storage and min-reduction

Pure CPU. No GPU, no SDL. Delivers the container and the reduction the GPU path will later mirror.

**Files:**
- Create: `include/lotus/scene/depth_pyramid.hpp`
- Create: `src/scene/depth_pyramid.cpp`
- Create: `test/occlusion_test.cpp`

**Interfaces:**
- Consumes: `lotus::BoundingSphere` from `include/lotus/scene/culling.hpp`.
- Produces: `class lotus::DepthPyramid` with `build`, `adopt`, `clear`, `empty`, `width`, `height`, `level_count`, `level_width`, `level_height`, `level`; plus `constexpr float lotus::kDepthFar` and `constexpr uint32_t lotus::kDepthPyramidMinLevel`. Task 2 adds `occluded`. Task 3 uses the class, Task 5 uses `adopt` and `kDepthPyramidMinLevel`.

- [ ] **Step 1: Write the failing test**

Create `test/occlusion_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/depth_pyramid.hpp"

using namespace lotus;

namespace {

/// Fills a w*h image with one value.
std::vector<float> flat_image(uint32_t w, uint32_t h, float value)
{
    return std::vector<float>(static_cast<std::size_t>(w) * h, value);
}

} // namespace

TEST(DepthPyramidTest, EmptyByDefault)
{
    const DepthPyramid pyramid;
    EXPECT_TRUE(pyramid.empty());
    EXPECT_EQ(pyramid.width(), 0u);
    EXPECT_EQ(pyramid.height(), 0u);
    EXPECT_EQ(pyramid.level_count(), 0u);
}

TEST(DepthPyramidTest, BuildStoresLevelZeroVerbatim)
{
    const std::vector<float> image = flat_image(8, 8, 3.0f);
    DepthPyramid pyramid;
    pyramid.build(image.data(), 8, 8);

    ASSERT_FALSE(pyramid.empty());
    EXPECT_EQ(pyramid.width(), 8u);
    EXPECT_EQ(pyramid.height(), 8u);
    EXPECT_EQ(pyramid.level(0), image);
}

TEST(DepthPyramidTest, LevelCountFollowsTheSmallerDimension)
{
    DepthPyramid square;
    square.build(flat_image(64, 64, 1.0f).data(), 64, 64);
    // 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1 is seven levels.
    EXPECT_EQ(square.level_count(), 7u);

    DepthPyramid wide;
    wide.build(flat_image(128, 32, 1.0f).data(), 128, 32);
    // 32 -> 16 -> 8 -> 4 -> 2 -> 1 bounds the chain.
    EXPECT_EQ(wide.level_count(), 6u);
}

TEST(DepthPyramidTest, HalvesEachLevelAndNeverGoesBelowOne)
{
    DepthPyramid pyramid;
    pyramid.build(flat_image(64, 64, 1.0f).data(), 64, 64);

    EXPECT_EQ(pyramid.level_width(0), 64u);
    EXPECT_EQ(pyramid.level_height(0), 64u);
    EXPECT_EQ(pyramid.level_width(1), 32u);
    EXPECT_EQ(pyramid.level_width(3), 8u);
    EXPECT_EQ(pyramid.level_width(6), 1u);
    EXPECT_EQ(pyramid.level_height(6), 1u);
}

TEST(DepthPyramidTest, EachLevelTakesTheMinimumOfTheBlockAbove)
{
    // 4x4, with the only small depth at the top-left texel.
    std::vector<float> image = flat_image(4, 4, kDepthFar);
    image[0] = 1.5f;
    image[1] = 9.0f;

    DepthPyramid pyramid;
    pyramid.build(image.data(), 4, 4);

    // Level 1 is 2x2; its (0,0) texel covers source texels (0,0),(1,0),(0,1),(1,1).
    const std::vector<float> level1 = pyramid.level(1);
    ASSERT_EQ(level1.size(), 4u);
    EXPECT_FLOAT_EQ(level1[0], 1.5f);
    // The remaining three 2x2 blocks see nothing but far values.
    EXPECT_FLOAT_EQ(level1[1], kDepthFar);
    EXPECT_FLOAT_EQ(level1[2], kDepthFar);
    EXPECT_FLOAT_EQ(level1[3], kDepthFar);

    // Level 2 is 1x1 and sees every texel of level 1, so it takes the minimum.
    const std::vector<float> level2 = pyramid.level(2);
    ASSERT_EQ(level2.size(), 1u);
    EXPECT_FLOAT_EQ(level2[0], 1.5f);
}

TEST(DepthPyramidTest, MinimumPropagatesAcrossManyLevels)
{
    std::vector<float> image = flat_image(128, 128, kDepthFar);
    image[127 * 128 + 127] = 2.25f; // bottom-right corner only

    DepthPyramid pyramid;
    pyramid.build(image.data(), 128, 128);

    const std::vector<float> coarsest = pyramid.level(pyramid.level_count() - 1);
    ASSERT_EQ(coarsest.size(), 1u);
    EXPECT_FLOAT_EQ(coarsest[0], 2.25f);
}

TEST(DepthPyramidTest, BuildRejectsNullAndZeroSizedInput)
{
    DepthPyramid pyramid;
    pyramid.build(flat_image(4, 4, 1.0f).data(), 4, 4);
    ASSERT_FALSE(pyramid.empty());

    pyramid.build(nullptr, 0, 0);
    EXPECT_TRUE(pyramid.empty());

    const float pixel = 1.0f;
    pyramid.build(&pixel, 8, 8);
    pyramid.build(&pixel, 0, 8);
    EXPECT_TRUE(pyramid.empty());
}

TEST(DepthPyramidTest, ClearResetsEverything)
{
    DepthPyramid pyramid;
    pyramid.build(flat_image(8, 8, 1.0f).data(), 8, 8);
    pyramid.clear();

    EXPECT_TRUE(pyramid.empty());
    EXPECT_EQ(pyramid.level_count(), 0u);
    EXPECT_TRUE(pyramid.level(0).empty());
}

TEST(DepthPyramidTest, AdoptInstallsGpuProducedMipsByLevel)
{
    // A 64x64 chain has seven levels; the renderer transfers 2..6.
    std::vector<std::vector<float>> mips(7);
    mips[2] = flat_image(16, 16, 4.0f);
    mips[3] = flat_image(8, 8, 4.0f);
    mips[4] = flat_image(4, 4, 4.0f);
    mips[5] = flat_image(2, 2, 4.0f);
    mips[6] = flat_image(1, 1, 4.0f);

    DepthPyramid pyramid;
    pyramid.adopt(64, 64, 7, mips);

    ASSERT_EQ(pyramid.level_count(), 7u);
    EXPECT_EQ(pyramid.width(), 64u);
    EXPECT_EQ(pyramid.level(2), mips[2]);
    EXPECT_EQ(pyramid.level(6), mips[6]);
    // Levels the renderer never transfers stay empty rather than fabricated.
    EXPECT_TRUE(pyramid.level(0).empty());
    EXPECT_TRUE(pyramid.level(1).empty());
}

TEST(DepthPyramidTest, AdoptRejectsShortOrEmptyUploads)
{
    DepthPyramid pyramid;

    pyramid.adopt(64, 64, 7, {}); // too few mips
    EXPECT_TRUE(pyramid.empty());

    pyramid.adopt(0, 0, 7, {}); // no dimensions
    EXPECT_TRUE(pyramid.empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -20`
Expected: FAIL — `fatal error: lotus/scene/depth_pyramid.hpp: No such file or directory`

- [ ] **Step 3: Write the header**

Create `include/lotus/scene/depth_pyramid.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/culling.hpp"

namespace lotus {

/// Depth written to texels that no geometry covered. Must exceed any camera far
/// plane: min-reduction treats it as "nothing here". An unwritten texel reads
/// as roughly zero, which min-reduction would propagate across the whole
/// pyramid and cull the entire scene.
constexpr float kDepthFar = 1.0e9f;

/// Finest mip the CPU keeps. Matches the renderer's readback floor. Mips above
/// this level are never transferred.
constexpr uint32_t kDepthPyramidMinLevel = 2;

/// CPU-side copy of a GPU-built hierarchical depth buffer ("Hi-Z").
///
/// Holds linear view-space depth: distance along the camera forward axis,
/// positive in front of the camera, in world units. Mip 0 is full resolution.
/// Each further mip stores the minimum of the 2x2 block above it, so a mip
/// answers "how far away is the farthest thing that could be here".
///
/// Deliberately GPU-free: this header includes no SDL header, which is what
/// keeps occlusion queryable in unit tests and keeps `Scene::build_vertices`
/// pure CPU.
class DepthPyramid {
public:
	/// Builds the chain from a tightly packed width*height image of linear
	/// depth. Ignores null or zero-sized input, leaving the pyramid empty.
	void build(const float* depth, uint32_t width, uint32_t height);

	void clear();

	bool empty() const { return width_ == 0 || height_ == 0; }
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }

	/// Number of mip levels including mip 0, or 0 when empty.
	uint32_t level_count() const
	{
		return static_cast<uint32_t>(levels_.size());
	}

	/// Dimensions of \p level, clamped to 1x1 at the coarsest level.
	uint32_t level_width(uint32_t level) const;
	uint32_t level_height(uint32_t level) const;

	/// Copies one mip out, rows tightly packed. Empty when \p level is out of
	/// range. Used by tests to inspect the chain.
	std::vector<float> level(uint32_t level) const;

	/// Replaces the chain with mips the GPU already reduced, indexed by level.
	///
	/// \p mips must have \p total_levels entries. Levels below
	/// kDepthPyramidMinLevel may be empty: the renderer never transfers them and
	/// occluded never reads them. This is the path that keeps depth.comp
	/// load-bearing, rather than re-reducing on the CPU.
	void adopt(uint32_t width, uint32_t height, uint32_t total_levels,
	           const std::vector<std::vector<float>>& mips);

private:
	std::vector<std::vector<float>> levels_;
	uint32_t width_{0};
	uint32_t height_{0};
};

} // namespace lotus
```

- [ ] **Step 4: Write the implementation**

Create `src/scene/depth_pyramid.cpp`:

```cpp
#include "lotus/scene/depth_pyramid.hpp"

#include <algorithm>
#include <cstddef>

namespace lotus {
namespace {

/// Halves \p value, never going below one.
uint32_t halve(uint32_t value)
{
	return value > 1 ? value / 2 : 1;
}

} // namespace

uint32_t DepthPyramid::level_width(uint32_t level) const
{
	// A shift of 32 or more is undefined, so short-circuit it.
	if (level >= 32) {
		return 1;
	}
	return std::max(1u, width_ >> level);
}

uint32_t DepthPyramid::level_height(uint32_t level) const
{
	if (level >= 32) {
		return 1;
	}
	return std::max(1u, height_ >> level);
}

std::vector<float> DepthPyramid::level(uint32_t index) const
{
	if (index >= levels_.size()) {
		return {};
	}
	return levels_[index];
}

void DepthPyramid::clear()
{
	levels_.clear();
	width_  = 0;
	height_ = 0;
}

void DepthPyramid::build(const float* depth, uint32_t width, uint32_t height)
{
	clear();
	if (depth == nullptr || width == 0 || height == 0) {
		return;
	}

	width_  = width;
	height_ = height;

	// The chain ends at 1x1, so its depth is set by the smaller dimension.
	uint32_t count = 1;
	for (uint32_t w = width, h = height; w > 1 || h > 1; ++count) {
		w = halve(w);
		h = halve(h);
	}

	levels_.resize(count);
	levels_[0].assign(depth, depth + static_cast<std::size_t>(width) * height);

	for (uint32_t level = 1; level < count; ++level) {
		const uint32_t src_w = level_width(level - 1);
		const uint32_t src_h = level_height(level - 1);
		const uint32_t dst_w = level_width(level);
		const uint32_t dst_h = level_height(level);

		const std::vector<float>& src = levels_[level - 1];
		std::vector<float>& dst      = levels_[level];
		dst.assign(static_cast<std::size_t>(dst_w) * dst_h, kDepthFar);

		for (uint32_t y = 0; y < dst_h; ++y) {
			for (uint32_t x = 0; x < dst_w; ++x) {
				// Odd dimensions leave a partial 2x2 block; the missing texels
				// stay at the far value.
				float nearest = kDepthFar;
				for (uint32_t dy = 0; dy < 2; ++dy) {
					const uint32_t sy = y * 2 + dy;
					if (sy >= src_h) {
						continue;
					}
					for (uint32_t dx = 0; dx < 2; ++dx) {
						const uint32_t sx = x * 2 + dx;
						if (sx >= src_w) {
							continue;
						}
						nearest = std::min(
						    nearest,
						    src[static_cast<std::size_t>(sy) * src_w + sx]);
					}
				}
				dst[static_cast<std::size_t>(y) * dst_w + x] = nearest;
			}
		}
	}
}


void DepthPyramid::adopt(uint32_t width, uint32_t height, uint32_t total_levels,
                        const std::vector<std::vector<float>>& mips)
{
	clear();
	if (width == 0 || height == 0 || total_levels == 0) {
		return;
	}
	if (mips.size() < total_levels) {
		return; // refuse a short upload rather than query half a chain
	}

	width_  = width;
	height_ = height;
	levels_.assign(total_levels, {});
	for (uint32_t level = kDepthPyramidMinLevel; level < total_levels; ++level) {
		levels_[level] = mips[level];
	}
}

} // namespace lotus
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='DepthPyramidTest.*'`
Expected: PASS, 10 tests.

Then confirm the baseline held:
Run: `./bin/lotus_tests 2>&1 | tail -3`
Expected: `[  PASSED  ] 115 tests.` (105 existing + 10 new)

- [ ] **Step 6: Commit**

```bash
git add include/lotus/scene/depth_pyramid.hpp src/scene/depth_pyramid.cpp test/occlusion_test.cpp
git commit -m "Add DepthPyramid: CPU-side Hi-Z chain with min-reduction"
```

---

### Task 2: The occlusion query

The part that actually culls. Still pure CPU and still fully unit-testable.

**Files:**
- Modify: `include/lotus/scene/depth_pyramid.hpp` (add `occluded`)
- Modify: `src/scene/depth_pyramid.cpp` (implement `occluded`)
- Modify: `test/occlusion_test.cpp`

**Interfaces:**
- Consumes: `DepthPyramid` from Task 1, `lotus::BoundingSphere`.
- Produces: `bool DepthPyramid::occluded(const glm::mat4& view, const glm::mat4& view_projection, const BoundingSphere& world_sphere) const`. Task 3 calls it.

- [ ] **Step 1: Write the failing tests**

Replace `test/occlusion_test.cpp` with this complete file. It keeps every Task 1 test and adds the query tests.

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "lotus/scene/depth_pyramid.hpp"

using namespace lotus;

namespace {

std::vector<float> flat_image(uint32_t w, uint32_t h, float value)
{
    return std::vector<float>(static_cast<std::size_t>(w) * h, value);
}

/// Camera at +Z looking at the origin. Matches Camera3D: right-handed lookAt
/// with a zero-to-one perspective projection.
struct TestCamera {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 view_projection;
};

TestCamera camera_from_z(float eye_z)
{
    TestCamera camera;
    camera.view = glm::lookAtRH(glm::vec3{0.0f, 0.0f, eye_z}, glm::vec3{0.0f},
                                glm::vec3{0.0f, 1.0f, 0.0f});
    camera.projection =
        glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    camera.view_projection = camera.projection * camera.view;
    return camera;
}

/// A 64x64 pyramid whose whole screen is a wall at linear depth \p depth.
DepthPyramid solid_wall(uint32_t size, float depth)
{
    DepthPyramid pyramid;
    const std::vector<float> image = flat_image(size, size, depth);
    pyramid.build(image.data(), size, size);
    return pyramid;
}

/// A 64x64 pyramid that is a wall of \p depth on the left half and far on the
/// right half, so a sphere straddling the seam must stay visible.
DepthPyramid split_wall(uint32_t size, float depth)
{
    std::vector<float> image(static_cast<std::size_t>(size) * size, kDepthFar);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size / 2; ++x) {
            image[static_cast<std::size_t>(y) * size + x] = depth;
        }
    }
    DepthPyramid pyramid;
    pyramid.build(image.data(), size, size);
    return pyramid;
}

} // namespace

// --- Task 1: storage and reduction (unchanged) ---

TEST(DepthPyramidTest, EmptyByDefault)
{
    const DepthPyramid pyramid;
    EXPECT_TRUE(pyramid.empty());
    EXPECT_EQ(pyramid.width(), 0u);
    EXPECT_EQ(pyramid.height(), 0u);
    EXPECT_EQ(pyramid.level_count(), 0u);
}

TEST(DepthPyramidTest, BuildStoresLevelZeroVerbatim)
{
    const std::vector<float> image = flat_image(8, 8, 3.0f);
    DepthPyramid pyramid;
    pyramid.build(image.data(), 8, 8);

    ASSERT_FALSE(pyramid.empty());
    EXPECT_EQ(pyramid.width(), 8u);
    EXPECT_EQ(pyramid.height(), 8u);
    EXPECT_EQ(pyramid.level(0), image);
}

TEST(DepthPyramidTest, LevelCountFollowsTheSmallerDimension)
{
    DepthPyramid square;
    square.build(flat_image(64, 64, 1.0f).data(), 64, 64);
    EXPECT_EQ(square.level_count(), 7u);

    DepthPyramid wide;
    wide.build(flat_image(128, 32, 1.0f).data(), 128, 32);
    EXPECT_EQ(wide.level_count(), 6u);
}

TEST(DepthPyramidTest, HalvesEachLevelAndNeverGoesBelowOne)
{
    DepthPyramid pyramid;
    pyramid.build(flat_image(64, 64, 1.0f).data(), 64, 64);

    EXPECT_EQ(pyramid.level_width(0), 64u);
    EXPECT_EQ(pyramid.level_height(0), 64u);
    EXPECT_EQ(pyramid.level_width(1), 32u);
    EXPECT_EQ(pyramid.level_width(3), 8u);
    EXPECT_EQ(pyramid.level_width(6), 1u);
    EXPECT_EQ(pyramid.level_height(6), 1u);
}

TEST(DepthPyramidTest, EachLevelTakesTheMinimumOfTheBlockAbove)
{
    std::vector<float> image = flat_image(4, 4, kDepthFar);
    image[0] = 1.5f;
    image[1] = 9.0f;

    DepthPyramid pyramid;
    pyramid.build(image.data(), 4, 4);

    const std::vector<float> level1 = pyramid.level(1);
    ASSERT_EQ(level1.size(), 4u);
    EXPECT_FLOAT_EQ(level1[0], 1.5f);
    EXPECT_FLOAT_EQ(level1[1], kDepthFar);
    EXPECT_FLOAT_EQ(level1[2], kDepthFar);
    EXPECT_FLOAT_EQ(level1[3], kDepthFar);

    const std::vector<float> level2 = pyramid.level(2);
    ASSERT_EQ(level2.size(), 1u);
    EXPECT_FLOAT_EQ(level2[0], 1.5f);
}

TEST(DepthPyramidTest, MinimumPropagatesAcrossManyLevels)
{
    std::vector<float> image = flat_image(128, 128, kDepthFar);
    image[127 * 128 + 127] = 2.25f;

    DepthPyramid pyramid;
    pyramid.build(image.data(), 128, 128);

    const std::vector<float> coarsest = pyramid.level(pyramid.level_count() - 1);
    ASSERT_EQ(coarsest.size(), 1u);
    EXPECT_FLOAT_EQ(coarsest[0], 2.25f);
}

TEST(DepthPyramidTest, BuildRejectsNullAndZeroSizedInput)
{
    DepthPyramid pyramid;
    pyramid.build(flat_image(4, 4, 1.0f).data(), 4, 4);
    ASSERT_FALSE(pyramid.empty());

    pyramid.build(nullptr, 0, 0);
    EXPECT_TRUE(pyramid.empty());

    const float pixel = 1.0f;
    pyramid.build(&pixel, 8, 8);
    pyramid.build(&pixel, 0, 8);
    EXPECT_TRUE(pyramid.empty());
}

TEST(DepthPyramidTest, ClearResetsEverything)
{
    DepthPyramid pyramid;
    pyramid.build(flat_image(8, 8, 1.0f).data(), 8, 8);
    pyramid.clear();

    EXPECT_TRUE(pyramid.empty());
    EXPECT_EQ(pyramid.level_count(), 0u);
    EXPECT_TRUE(pyramid.level(0).empty());
}

// --- Task 2: the query ---

TEST(DepthPyramidOcclusion, EmptyPyramidNeverOccludes)
{
    const DepthPyramid empty;
    const TestCamera camera = camera_from_z(10.0f);
    EXPECT_FALSE(empty.occluded(camera.view, camera.view_projection,
                                {{0.0f, 0.0f, 0.0f}, 1.0f}));
}

TEST(DepthPyramidOcclusion, ZeroRadiusNeverOccludes)
{
    const DepthPyramid pyramid = solid_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{0.0f, 0.0f, 0.0f}, 0.0f}));
}

TEST(DepthPyramidOcclusion, SphereBehindAWallIsOccluded)
{
    const DepthPyramid pyramid = solid_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);

    // Camera at z=10, so this sphere's nearest point is at depth 9.4.
    EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
                                 {{0.0f, 0.0f, 0.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, SphereInFrontOfTheWallIsVisible)
{
    const DepthPyramid pyramid = solid_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);

    // At z=8 the nearest point is at depth 1.9, well in front of the wall.
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{0.0f, 0.0f, 8.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, SphereStraddlingTheNearPlaneIsVisible)
{
    const DepthPyramid pyramid = solid_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);

    // Radius 0.5 at z=9.95 reaches past the camera plane.
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{0.0f, 0.0f, 9.95f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, SphereStraddlingTheWallEdgeIsVisible)
{
    const DepthPyramid pyramid = split_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);

    // NDC x is world x divided by depth times tan(30deg). At depth 10 that
    // puts x = 2.9 near the right edge of a 64-wide image, which is where the
    // wall ends, so the footprint covers both wall and empty space.
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{2.9f, 0.0f, 0.0f}, 0.4f}));
}

TEST(DepthPyramidOcclusion, SphereFullyBehindTheWallHalfIsOccluded)
{
    const DepthPyramid pyramid = split_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);

    EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
                                 {{-3.0f, 0.0f, 0.0f}, 0.4f}));
}

TEST(DepthPyramidOcclusion, SphereOffToTheSideIsVisible)
{
    const DepthPyramid pyramid = solid_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);

    // Far outside the frustum in x and y, so never culled even behind a wall.
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{100.0f, 0.0f, 0.0f}, 1.0f}));
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{0.0f, 100.0f, 0.0f}, 1.0f}));
}

TEST(DepthPyramidOcclusion, SphereBehindTheCameraIsVisible)
{
    const DepthPyramid pyramid = solid_wall(64, 5.0f);
    const TestCamera camera    = camera_from_z(10.0f);

    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{0.0f, 0.0f, 20.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, LargeSphereSpanningManyTexelsStaysConservative)
{
    // A wall only in the middle 8x8 texels; anything larger than that must
    // survive because its footprint reaches past the occluder.
    std::vector<float> image = flat_image(64, 64, kDepthFar);
    for (uint32_t y = 28; y < 36; ++y) {
        for (uint32_t x = 28; x < 36; ++x) {
            image[static_cast<std::size_t>(y) * 64 + x] = 5.0f;
        }
    }
    DepthPyramid pyramid;
    pyramid.build(image.data(), 64, 64);
    const TestCamera camera = camera_from_z(10.0f);

    // Small enough to sit inside the wall.
    EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
                                 {{0.0f, 0.0f, 0.0f}, 0.15f}));
    // Large enough that its footprint spills outside the wall.
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{0.0f, 0.0f, 0.0f}, 4.0f}));
}

TEST(DepthPyramidOcclusion, UntouchedTexelsDoNotCullTheScene)
{
    // This is the failure mode the far clear exists to prevent: if an unwritten
    // texel were zero, min-reduction would spread that zero everywhere.
    const DepthPyramid pyramid = solid_wall(64, kDepthFar);
    const TestCamera camera    = camera_from_z(10.0f);

    // An all-far pyramid has nothing in front, so nothing is occluded.
    EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
                                  {{0.0f, 0.0f, 0.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, ClassificationFollowsCameraDistance)
{
    // The pyramid stores linear depth measured from whichever camera built it.
    // A wall at depth 5 from a camera at z=10 sits at world z=5.
    const DepthPyramid pyramid = solid_wall(64, 5.0f);

    // From z=10 the sphere at world z=0 is 10 units away, behind the wall.
    const TestCamera camera = camera_from_z(10.0f);
    EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
                                 {{0.0f, 0.0f, 0.0f}, 0.5f}));

    // From z=7 the same sphere is only 7 units away, still behind the wall.
    const TestCamera closer = camera_from_z(7.0f);
    EXPECT_TRUE(pyramid.occluded(closer.view, closer.view_projection,
                                 {{0.0f, 0.0f, 0.0f}, 0.5f}));

    // From z=3 the sphere is 3 units away, in front of the depth-5 wall. The
    // pyramid is stale relative to this camera, but the test stays honest
    // because it compares view-space depths rather than reusing frame state.
    const TestCamera closest = camera_from_z(3.0f);
    EXPECT_FALSE(pyramid.occluded(closest.view, closest.view_projection,
                                  {{0.0f, 0.0f, 0.0f}, 0.5f}));
}

TEST(DepthPyramidTest, AdoptInstallsGpuProducedMipsByLevel)
{
    // A 64x64 chain has seven levels; the renderer transfers 2..6.
    std::vector<std::vector<float>> mips(7);
    mips[2] = flat_image(16, 16, 4.0f);
    mips[3] = flat_image(8, 8, 4.0f);
    mips[4] = flat_image(4, 4, 4.0f);
    mips[5] = flat_image(2, 2, 4.0f);
    mips[6] = flat_image(1, 1, 4.0f);

    DepthPyramid pyramid;
    pyramid.adopt(64, 64, 7, mips);

    ASSERT_EQ(pyramid.level_count(), 7u);
    EXPECT_EQ(pyramid.width(), 64u);
    EXPECT_EQ(pyramid.level(2), mips[2]);
    EXPECT_EQ(pyramid.level(6), mips[6]);
    // Levels the renderer never transfers stay empty rather than fabricated.
    EXPECT_TRUE(pyramid.level(0).empty());
    EXPECT_TRUE(pyramid.level(1).empty());
}

TEST(DepthPyramidTest, AdoptRejectsShortOrEmptyUploads)
{
    DepthPyramid pyramid;

    pyramid.adopt(64, 64, 7, {}); // too few mips
    EXPECT_TRUE(pyramid.empty());

    pyramid.adopt(0, 0, 7, {}); // no dimensions
    EXPECT_TRUE(pyramid.empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -20`
Expected: FAIL — `error: no member named 'occluded' in 'lotus::DepthPyramid'`

- [ ] **Step 3: Declare `occluded` in the header**

In `include/lotus/scene/depth_pyramid.hpp`, inside the class, insert after the `level` declaration:

```cpp
	/// True when \p world_sphere is entirely behind everything already drawn.
	///
	/// Conservative by construction: false means "draw it", which may still turn
	/// out to be occluded. True is only returned when the sphere's nearest point
	/// is behind the farthest depth in its screen footprint, so a partially
	/// visible sphere is never culled.
	///
	/// \p view maps world space to view space; \p view_projection must use the
	/// zero-to-one depth convention that Camera3D produces, matching Frustum.
	bool occluded(const glm::mat4& view, const glm::mat4& view_projection,
	              const BoundingSphere& world_sphere) const;
```

- [ ] **Step 4: Implement `occluded`**

Add to the top of `src/scene/depth_pyramid.cpp`'s anonymous namespace:

```cpp
/// Grows the sphere so it does not occlude itself and then flicker as the
/// camera moves.
constexpr float kRadiusBias = 1.02f;

/// Clamps \p value into [lo, hi].
uint32_t clamp_index(uint32_t value, uint32_t lo, uint32_t hi)
{
	return value < lo ? lo : (value > hi ? hi : value);
}
```

Add `#include <cmath>` and `#include <limits>` to the includes, then append before the closing `} // namespace lotus`:

```cpp
bool DepthPyramid::occluded(const glm::mat4& view,
                            const glm::mat4& view_projection,
                            const BoundingSphere& world_sphere) const
{
	if (empty() || world_sphere.radius <= 0.0f) {
		return false;
	}

	const uint32_t count = level_count();
	if (count == 0) {
		return false;
	}

	const float radius = world_sphere.radius * kRadiusBias;

	// View-space centre, and the sphere's nearest extent along the view axis.
	const glm::vec3 view_center{view * glm::vec4{world_sphere.center, 1.0f}};
	const float nearest_depth = -view_center.z - radius;
	if (nearest_depth <= 0.0f) {
		return false; // straddles or crosses the camera plane
	}

	// Project the eight corners of the sphere's view-space AABB. Including the
	// z extremes widens the footprint, which is the conservative direction.
	glm::vec2 screen_min{std::numeric_limits<float>::max(),
	                    std::numeric_limits<float>::max()};
	glm::vec2 screen_max{std::numeric_limits<float>::lowest(),
	                    std::numeric_limits<float>::lowest()};
	for (int corner = 0; corner < 8; ++corner) {
		const glm::vec3 offset{(corner & 1) ? radius : -radius,
		                       (corner & 2) ? radius : -radius,
		                       (corner & 4) ? radius : -radius};
		const glm::vec4 clip =
		        view_projection * glm::vec4{view_center + offset, 1.0f};
		if (clip.w <= 0.0f) {
			return false;
		}
		const glm::vec3 ndc{clip.xyz / clip.w};
		screen_min = glm::min(screen_min, glm::vec2{ndc.x, ndc.y});
		screen_max = glm::max(screen_max, glm::vec2{ndc.x, ndc.y});
	}

	// NDC to pixels.
	const float width_f  = static_cast<float>(width_);
	const float height_f = static_cast<float>(height_);
	const float min_x = (screen_min.x * 0.5f + 0.5f) * width_f;
	const float max_x = (screen_max.x * 0.5f + 0.5f) * width_f;
	const float min_y = (screen_min.y * 0.5f + 0.5f) * height_f;
	const float max_y = (screen_max.y * 0.5f + 0.5f) * height_f;

	// Outside the viewport: leave it to the frustum test.
	if (max_x < 0.0f || min_x > width_f || max_y < 0.0f || min_y > height_f) {
		return false;
	}

	const float clamped_min_x = std::max(0.0f, min_x);
	const float clamped_max_x = std::min(width_f, max_x);
	const float clamped_min_y = std::max(0.0f, min_y);
	const float clamped_max_y = std::min(height_f, max_y);

	// Coarsest level whose texel grid is still coarser than the footprint, so
	// the whole footprint lands in one texel at that level.
	const float span = std::max(clamped_max_x - clamped_min_x,
	                            clamped_max_y - clamped_min_y);
	uint32_t level = 0;
	if (span > 1.0f) {
		level = static_cast<uint32_t>(std::ceil(std::log2(span)));
	}

	const uint32_t last = count - 1;
	if (last < kDepthPyramidMinLevel) {
		level = last;
	} else {
		level = clamp_index(level, kDepthPyramidMinLevel, last);
	}

	const uint32_t mip_w = level_width(level);
	const uint32_t mip_h = level_height(level);
	const float scale_x = static_cast<float>(mip_w) / width_f;
	const float scale_y = static_cast<float>(mip_h) / height_f;

	uint32_t x0 = static_cast<uint32_t>(
	    std::max(0.0f, std::floor(clamped_min_x * scale_x)));
	uint32_t y0 = static_cast<uint32_t>(
	    std::max(0.0f, std::floor(clamped_min_y * scale_y)));
	uint32_t x1 = static_cast<uint32_t>(
	    std::min(static_cast<float>(mip_w), std::ceil(clamped_max_x * scale_x)));
	uint32_t y1 = static_cast<uint32_t>(
	    std::min(static_cast<float>(mip_h), std::ceil(clamped_max_y * scale_y)));

	x0 = std::min(x0, mip_w - 1);
	y0 = std::min(y0, mip_h - 1);
	x1 = std::max(x1, x0 + 1);
	y1 = std::max(y1, y0 + 1);

	// The farthest occluder anywhere under the footprint. Using the maximum is
	// what makes the test conservative: the sphere must beat even the most
	// distant occluder covering it to be called hidden.
	const std::vector<float>& mip = levels_[level];
	float farthest = kDepthFar;
	for (uint32_t y = y0; y < y1; ++y) {
		const std::size_t row = static_cast<std::size_t>(y) * mip_w;
		for (uint32_t x = x0; x < x1; ++x) {
			farthest = std::max(farthest, mip[row + x]);
		}
	}

	return nearest_depth >= farthest;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='DepthPyramid*'`
Expected: PASS, all `DepthPyramidTest.*` and `DepthPyramidOcclusion.*`.

If `ClassificationFollowsCameraDistance` fails, the wall depth and sphere depth are inconsistent: re-check that a camera at `eye_z` puts world `z` at linear depth `eye_z - z`.

Run: `./bin/lotus_tests 2>&1 | tail -3`
Expected: `[  PASSED  ] 127 tests.`

- [ ] **Step 6: Commit**

```bash
git add include/lotus/scene/depth_pyramid.hpp src/scene/depth_pyramid.cpp test/occlusion_test.cpp
git commit -m "Add conservative bounding-sphere occlusion query"
```

---

### Task 3: Scene integration

Threads the pyramid into culling and exposes the counter. No GPU involved, so this is testable on its own.

**Files:**
- Modify: `include/lotus/scene/components.hpp` (add `Camera3D::view()`, `Camera2D::view()`)
- Modify: `include/lotus/scene/scene.hpp`
- Modify: `src/scene/scene.cpp`
- Modify: `test/occlusion_test.cpp`

**Interfaces:**
- Consumes: `DepthPyramid::occluded` from Task 2.
- Produces: `std::vector<GpuVertex> Scene::build_vertices(float aspect, const DepthPyramid* occlusion = nullptr)`, `uint32_t Scene::occluded_entities() const`, `lotus::Camera3D::view()`, `lotus::Camera2D::view()`. Task 6 reads `occluded_entities()`.

- [ ] **Step 1: Write the failing scene test**

Append to `test/occlusion_test.cpp`. Add these includes at the top of the file:

```cpp
#include "lotus/scene/scene.hpp"
```

Append these tests after the existing ones:

```cpp
TEST(SceneOcclusionTest, OccludedEntityLeavesTheVertexBuffer)
{
    Scene scene{"s"};
    const Entity camera = scene.create("camera");
    scene.registry().emplace<Camera3D>(camera, Camera3D{{0.0f, 0.0f, 10.0f},
                                                       {0.0f, 0.0f, 0.0f},
                                                       {0.0f, 1.0f, 0.0f},
                                                       60.0f, 0.1f, 100.0f});

    const Entity cube = scene.create("cube");
    scene.registry().emplace<MeshSource>(cube, MeshSource{"builtin:cube"});
    scene.update(0.0f);

    // A wall covering the whole screen at linear depth 5.
    const DepthPyramid pyramid = solid_wall(64, 5.0f);

    const std::vector<GpuVertex> drawn =
        scene.build_vertices(1.0f, &pyramid);

    EXPECT_TRUE(drawn.empty());
    EXPECT_EQ(scene.occluded_entities(), 1u);
    EXPECT_EQ(scene.culled_entities(), 1u);
    EXPECT_EQ(scene.drawn_entities(), 0u);
}

TEST(SceneOcclusionTest, VisibleEntitySurvivesTheSamePyramid)
{
    Scene scene{"s"};
    const Entity camera = scene.create("camera");
    scene.registry().emplace<Camera3D>(camera, Camera3D{{0.0f, 0.0f, 10.0f},
                                                       {0.0f, 0.0f, 0.0f},
                                                       {0.0f, 1.0f, 0.0f},
                                                       60.0f, 0.1f, 100.0f});

    const Entity cube = scene.create("cube");
    scene.registry().emplace<MeshSource>(cube, MeshSource{"builtin:cube"});
    // In front of the depth-5 wall.
    scene.registry().get<Transform>(cube).position = {0.0f, 0.0f, 8.0f};
    scene.update(0.0f);

    const DepthPyramid pyramid = solid_wall(64, 5.0f);
    const std::vector<GpuVertex> drawn = scene.build_vertices(1.0f, &pyramid);

    EXPECT_EQ(drawn.size(), 36u); // a cube is 12 triangles
    EXPECT_EQ(scene.drawn_entities(), 1u);
    EXPECT_EQ(scene.occluded_entities(), 0u);
    EXPECT_EQ(scene.culled_entities(), 0u);
}

TEST(SceneOcclusionTest, NullPyramidMatchesTodayExactly)
{
    Scene scene{"s"};
    const Entity camera = scene.create("camera");
    scene.registry().emplace<Camera3D>(camera);

    const Entity cube = scene.create("cube");
    scene.registry().emplace<MeshSource>(cube, MeshSource{"builtin:cube"});
    scene.update(0.0f);

    const std::vector<GpuVertex> with_null = scene.build_vertices(1.0f, nullptr);
    const std::vector<GpuVertex> defaulted = scene.build_vertices(1.0f);

    ASSERT_EQ(with_null.size(), defaulted.size());
    EXPECT_EQ(scene.occluded_entities(), 0u);
    EXPECT_FALSE(with_null.empty());
}

TEST(SceneOcclusionTest, EmptyPyramidCullsNothing)
{
    Scene scene{"s"};
    const Entity camera = scene.create("camera");
    scene.registry().emplace<Camera3D>(camera);

    const Entity cube = scene.create("cube");
    scene.registry().emplace<MeshSource>(cube, MeshSource{"builtin:cube"});
    scene.update(0.0f);

    const DepthPyramid empty;
    const std::vector<GpuVertex> drawn = scene.build_vertices(1.0f, &empty);

    EXPECT_EQ(drawn.size(), 36u);
    EXPECT_EQ(scene.occluded_entities(), 0u);
}
```

`solid_wall` lives in the anonymous namespace already added in Task 2, so it is available here.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -20`
Expected: FAIL — `error: too many arguments to 'build_vertices'` and `'occluded_entities' is not a member of 'lotus::Scene'`

- [ ] **Step 3: Add `view()` to both cameras**

In `include/lotus/scene/components.hpp`, replace the `Camera3D::view_projection` body:

```cpp
	glm::mat4 view() const
	{
		return glm::lookAtRH(position, target, up);
	}

	glm::mat4 view_projection(float aspect) const
	{
		const glm::mat4 projection = glm::perspectiveRH_ZO(
		        glm::radians(fov_degrees), aspect > 0.0f ? aspect : 1.0f,
		        near_plane, far_plane);
		return projection * view();
	}
```

And replace the `Camera2D::view_projection` body:

```cpp
	glm::mat4 view() const
	{
		return glm::translate(glm::mat4{1.0f}, glm::vec3{-position, 0.0f});
	}

	glm::mat4 view_projection(float aspect) const
	{
		const float half_h = 1.0f / zoom;
		const float half_w = half_h * aspect;
		const glm::mat4 proj =
		        glm::ortho(-half_w, half_w, -half_h, half_h, -1.0f, 1.0f);
		return proj * view();
	}
```

- [ ] **Step 4: Update the Scene header**

In `include/lotus/scene/scene.hpp`:

Replace the `build_vertices` declaration:

```cpp
	/// Resolves the camera, culls against the view frustum and \p occlusion,
	/// picks LOD levels, and returns this frame's vertices. Pure CPU work: no
	/// GPU needed, which is what makes culling and LOD testable. Passing a null
	/// or empty pyramid disables occlusion culling.
	std::vector<GpuVertex> build_vertices(float aspect,
	                                      const DepthPyramid* occlusion = nullptr);
```

Add after the `culled_entities` accessor:

```cpp
	/// Entities rejected last frame by occlusion, after passing frustum and LOD.
	uint32_t occluded_entities() const { return occluded_entities_; }
```

Add the forward declaration after `class GpuRenderer;`:

```cpp
class DepthPyramid;
```

Add the member after `uint32_t culled_entities_{0};`:

```cpp
	uint32_t occluded_entities_{0};
	glm::mat4 view_{1.0f};
```

- [ ] **Step 5: Wire the query into `build_vertices`**

In `src/scene/scene.cpp`, add `#include "lotus/scene/depth_pyramid.hpp"` after the `lod.hpp` include.

Add a helper to the anonymous namespace:

```cpp
/// True when \p pyramid reports this sphere as hidden. A null pyramid means
/// occlusion is disabled, which must never cull anything.
bool hidden(const DepthPyramid* pyramid, const glm::mat4& view,
            const glm::mat4& view_projection, const BoundingSphere& world)
{
	return pyramid != nullptr && pyramid->occluded(view, view_projection, world);
}
```

Change the signature and resolve the view matrix:

```cpp
std::vector<GpuVertex> Scene::build_vertices(float aspect,
                                              const DepthPyramid* occlusion)
{
	// Resolve the camera once, then reuse it for culling and drawing.
	if (const Camera3D* camera = active_camera3d()) {
		view_            = camera->view();
		view_projection_ = camera->view_projection(aspect);
		camera_position_ = camera->position;
	} else {
		static const Camera2D fallback{};
		const Camera2D* camera2d = active_camera();
		const Camera2D& chosen   = camera2d ? *camera2d : fallback;
		view_                   = chosen.view();
		view_projection_        = chosen.view_projection(aspect);
		camera_position_        = glm::vec3{chosen.position, 1.0f};
	}
	const Frustum frustum = Frustum::from_view_projection(view_projection_);

	std::vector<GpuVertex> vertices;
	drawn_entities_   = 0;
	culled_entities_  = 0;
	occluded_entities_ = 0;
```

In the sprite loop, replace the frustum-only check with:

```cpp
		const BoundingSphere world = world_bounds(transform, local);
		if (!frustum.intersects(world)) {
			++culled_entities_;
			return;
		}
		if (hidden(occlusion, view_, view_projection_, world)) {
			++occluded_entities_;
			++culled_entities_;
			return;
		}
```

In the plain-mesh loop, replace the frustum-only check with:

```cpp
		const BoundingSphere world = world_bounds(transform, mesh.bounds);
		if (!frustum.intersects(world)) {
			++culled_entities_;
			return;
		}
		if (hidden(occlusion, view_, view_projection_, world)) {
			++occluded_entities_;
			++culled_entities_;
			return;
		}
```

In the LOD loop, replace the frustum-only check after level selection with:

```cpp
		const BoundingSphere world = world_bounds(transform, mesh.bounds);
		if (!frustum.intersects(world)) {
			++culled_entities_;
			return;
		}
		if (hidden(occlusion, view_, view_projection_, world)) {
			++occluded_entities_;
			++culled_entities_;
			return;
		}
```

Finally, replace the body of `Scene::upload` so it forwards the renderer's
pyramid. The existing body is `const std::vector<GpuVertex> vertices =
build_vertices(aspect);` followed by the `vertex_count_` assignment and the
`gpu.upload_vertices` call; change only the `build_vertices` argument:

```cpp
void Scene::upload(GpuRenderer& gpu, float aspect)
{
	const std::vector<GpuVertex> vertices =
	        build_vertices(aspect, &gpu.occlusion_pyramid());
	vertex_count_ = static_cast<std::uint32_t>(vertices.size());
	if (vertex_count_ != 0) {
		gpu.upload_vertices(vertices.data(), vertex_count_);
	}
}
```

No new member is introduced; `upload` keeps its local.

- [ ] **Step 6: Build and run**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -20`

`scene.cpp` includes `lotus/gpu/renderer.hpp` already, so `gpu.occlusion_pyramid()` resolves once Task 5 adds it. Until then, declare the accessor now so Task 3 builds standalone — add to `include/lotus/gpu/renderer.hpp`:

```cpp
	/// CPU copy of the previous frame's depth pyramid. Never null; empty when
	/// the fence has not landed yet, in which case occlusion is skipped.
	const DepthPyramid& occlusion_pyramid() const { return occlusion_pyramid_; }
```

and add `#include "lotus/scene/depth_pyramid.hpp"` plus the member `DepthPyramid occlusion_pyramid_;` to that header. Task 5 fills in the GPU side of this member; the accessor is the seam both tasks share.

Run: `./bin/lotus_tests --gtest_filter='SceneOcclusion*'`
Expected: PASS, 4 tests.

Run: `./bin/lotus_tests 2>&1 | tail -3`
Expected: `[  PASSED  ] 131 tests.` If any `SceneCullingTest` or `SceneLodTest` fails, the frustum or LOD path regressed — fix before continuing.

- [ ] **Step 7: Commit**

```bash
git add include/lotus/scene/components.hpp include/lotus/scene/scene.hpp \
        include/lotus/gpu/renderer.hpp src/scene/scene.cpp test/occlusion_test.cpp
git commit -m "Thread the depth pyramid through Scene culling"
```

---

### Task 4: Shaders and the SPIR-V toolchain

Adds the linear-depth output and the reduction shader, then regenerates the committed headers. Verifiable without running the engine.

**Files:**
- Create: `shaders/depth.comp`
- Modify: `shaders/lit.vert`, `shaders/lit.frag`, `shaders/build-shaders.sh`
- Regenerate: `include/lotus/gpu/lit.vert.spv.h`, `include/lotus/gpu/lit.frag.spv.h`, `include/lotus/gpu/depth.comp.spv.h`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `lit_vert_spv`, `lit_frag_spv`, `depth_comp_spv` symbols in `include/lotus/gpu/*.spv.h`. Task 5 binds them.

- [ ] **Step 1: Extend `lit.vert`**

Replace the file with:

```glsl
#version 450

// Lit geometry vertex stage.
//
// Geometry is uploaded already transformed into world space (the CPU applies
// the model matrix when building the batch), so the model matrix here is
// identity and the input position *is* the world position. That keeps the
// whole scene in a single draw call.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;

layout(set = 1, binding = 0) uniform VertexUniforms {
    mat4 view_projection;
    mat4 view;
} u;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_color;

// View-space position, for writing linear depth to the second colour target.
layout(location = 3) out vec3 v_view_position;

void main()
{
    v_world_position = in_position;
    v_normal         = in_normal;
    v_color          = in_color;
    v_view_position  = (u.view * vec4(in_position, 1.0)).xyz;
    gl_Position      = u.view_projection * vec4(in_position, 1.0);
}
```

- [ ] **Step 2: Extend `lit.frag`**

Add a second output after the existing `out_color` declaration:

```glsl
// Linear view depth, feeding the Hi-Z pyramid. Distance along the camera
// forward axis, in world units. Cleared to 1e9 each frame; must match
// lotus::kDepthFar.
layout(location = 1) out vec4 out_linear_depth;
```

and add the write at the end of `main`, after `out_color = vec4(lit, 1.0);`:

```glsl
    out_linear_depth = vec4(-v_view_position.z, 0.0, 0.0, 1.0);
```

- [ ] **Step 3: Create `shaders/depth.comp`**

```glsl
#version 450

// Hierarchical Z reduction: one output level per dispatch. Every texel of dst
// takes the minimum of the 2x2 block of src above it.
//
// Min, not average. Averaging blurs occluder boundaries, and a blurred
// boundary makes geometry that is still partly visible look hidden, which is
// the one failure this whole feature must never produce.

layout(local_size_x = 8, local_size_y = 8) in;

// Both slots are bound as read-write storage textures by
// SDL_BeginGPUComputePass, because SDL_GPUStorageTextureReadWriteBinding is
// the only SDL binding that carries a mip_level. The two addresses never
// overlap within a dispatch, so simultaneous access is safe.
// GLSL 4.50 rejects an image that is neither readonly/writeonly nor carries
// a format qualifier, so both spell out r32f to match the R32_FLOAT texture.
layout(set = 0, binding = 0, r32f) readonly uniform image2D src;
layout(set = 0, binding = 1, r32f) writeonly uniform image2D dst;

void main()
{
    const ivec2 dst_size = imageSize(dst);
    const ivec2 position  = ivec2(gl_GlobalInvocationID.xy);
    if (position.x >= dst_size.x || position.y >= dst_size.y) {
        return;
    }

    const ivec2 src_size = imageSize(src);
    const ivec2 base      = position * 2;

    // Must match lotus::kDepthFar. Odd dimensions leave a partial 2x2 block,
    // and the missing texels must read as "nothing here", not as nearest.
    float nearest = 1.0e9;

    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const ivec2 texel = base + ivec2(dx, dy);
            if (texel.x >= src_size.x || texel.y >= src_size.y) {
                continue;
            }
            nearest = min(nearest, imageLoad(src, texel).r);
        }
    }

    imageStore(dst, position, vec4(nearest, 0.0, 0.0, 1.0));
}
```

- [ ] **Step 4: Extend `build-shaders.sh` to cover `comp`**

Replace the whole of `shaders/build-shaders.sh` with the script below. The loop
is a list of `<stage> <base>` pairs because the original hardcoded
`source="$here/lit.$stage"`, which would look for `lit.comp` instead of
`depth.comp`.
```bash
#!/usr/bin/env bash
#
# Compiles the engine's GLSL shaders to SPIR-V and writes them as C headers
# under include/lotus/gpu/. The generated headers are committed, so a normal
# build never needs a shader compiler.
#
# Requires glslangValidator (Fedora: `dnf install glslang`, or point GLSLANG
# at a copy extracted from the RPM when you cannot install packages):
#
#   GLSLANG=/tmp/shadertools/usr/bin/glslangValidator ./shaders/build-shaders.sh
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
out_dir="$root/include/lotus/gpu"
glslang="${GLSLANG:-glslangValidator}"

if ! command -v "$glslang" >/dev/null 2>&1; then
    echo "error: glslangValidator not found (set GLSLANG=/path/to/glslangValidator)" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# "<stage> <base>": lit supplies vert and frag, depth supplies comp.
for pair in "vert lit" "frag lit" "comp depth"; do
    stage="${pair%% *}"
    base="${pair##* }"
    source="$here/$base.$stage"
    spv="$work/$base.$stage.spv"
    header="$out_dir/$base.$stage.spv.h"
    name="${base}_${stage}_spv"

    "$glslang" -V "$source" -o "$spv" >/dev/null

    python3 - "$spv" "$name" "$header" "$base.$stage" <<'PY'
import sys

spv_path, name, header_path, source_name = sys.argv[1:5]
data = open(spv_path, "rb").read()

lines = [
    "#pragma once",
    f"// Generated from shaders/{source_name} by shaders/build-shaders.sh. Do not edit.",
    f"static const unsigned char {name}[] = {{",
]
for i in range(0, len(data), 12):
    chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
    lines.append(f"  {chunk},")
lines.append("};")
lines.append(f"static const unsigned int {name}_len = {len(data)};")
lines.append("")

open(header_path, "w").write("\n".join(lines))
PY

    echo "wrote $header"
done
```

- [ ] **Step 5: Regenerate the headers and verify**

Run: `cd /home/jlo/dev/LotusEngine && GLSLANG=/tmp/shadertools/usr/bin/glslangValidator ./shaders/build-shaders.sh`
Expected:
```
wrote .../include/lotus/gpu/lit.vert.spv.h
wrote .../include/lotus/gpu/lit.frag.spv.h
wrote .../include/lotus/gpu/depth.comp.spv.h
```

Verify the generated comment names the right source file and the symbols exist:
Run: `grep -h 'Generated from\|_len = ' include/lotus/gpu/*.spv.h`
Expected: three `Generated from` lines naming `lit.vert`, `lit.frag`, `depth.comp`, and three `_len` lines including `depth_comp_spv_len`.

If `/tmp/shadertools/usr/bin/glslangValidator` is missing, extract it first:

```bash
dnf repoquery --location glslang
dnf download --destdir=/tmp/glslang-rpm glslang-16.2.0-1.fc44.x86_64
rpm2cpio /tmp/glslang-rpm/glslang-*.rpm | cpio -idm -D /tmp/shadertools
```

- [ ] **Step 6: Commit**

```bash
git add shaders/lit.vert shaders/lit.frag shaders/depth.comp shaders/build-shaders.sh \
        include/lotus/gpu/lit.vert.spv.h include/lotus/gpu/lit.frag.spv.h \
        include/lotus/gpu/depth.comp.spv.h
git commit -m "Emit linear view depth and add the Hi-Z reduction shader"
```

---

### Task 5: Renderer GPU path

The largest task. Builds the pyramid on the GPU, downloads it without blocking, and hands it to the CPU.

**Files:**
- Modify: `include/lotus/gpu/renderer.hpp`
- Modify: `src/gpu/renderer.cpp`

**Interfaces:**
- Consumes: `depth_comp_spv` from Task 4, `DepthPyramid` from Task 1, the `occlusion_pyramid()` accessor already added in Task 3.
- Produces: `GpuRenderer::occlusion_enabled()`, `GpuRenderer::occlusion_ready()`. Task 6 reads both.

- [ ] **Step 1: Add the header members and declarations**

In `include/lotus/gpu/renderer.hpp`, after the existing `draw`/`end_frame` declarations, add:

```cpp
	/// False when compute shaders or R32 storage are unavailable. Culling then
	/// falls back to frustum and LOD only, with no errors.
	bool occlusion_enabled() const { return occlusion_enabled_; }

	/// True once a downloaded pyramid has landed. False means this frame skips
	/// occlusion because the previous frame's fence is still outstanding.
	bool occlusion_ready() const { return occlusion_ready_; }
```

In the private section add:

```cpp
	bool create_occlusion_pipeline();
	void destroy_occlusion();
	void ensure_pyramid(uint32_t width, uint32_t height);
	void poll_pyramid_fence();
	void record_pyramid();
	uint32_t pyramid_level_count(uint32_t width, uint32_t height) const;
```

Add the members next to the existing depth members:

```cpp
	SDL_GPUTexture* pyramid_texture_{nullptr};
	uint32_t pyramid_width_{0};
	uint32_t pyramid_height_{0};
	uint32_t pyramid_levels_{0};
	std::vector<uint32_t> pyramid_offset_; // byte offset of each readable mip
	SDL_GPUTransferBuffer* pyramid_transfer_{nullptr};
	SDL_GPUFence* pyramid_fence_{nullptr};
	bool pyramid_in_flight_{false};
	SDL_GPUComputePipeline* hiz_pipeline_{nullptr};
	bool occlusion_enabled_{false};
	bool occlusion_ready_{false};
```

Add `#include <vector>` if it is not already included.

- [ ] **Step 2: Implement the resource lifecycle**

Add to `src/gpu/renderer.cpp`, near the other constants:

```cpp
/// Rows in the readback transfer buffer are padded so each pitch is a multiple
/// of this. D3D12 requires 256; matching it everywhere avoids SDL inserting a
/// repacking copy on every download.
constexpr uint32_t kTransferRowAlignment = 256;
```

Implement:

```cpp
// Must match DepthPyramid::build exactly: the chain ends when the SMALLER
// dimension reaches 1. Vulkan's own convention would keep halving until the
// larger dimension reaches 1, which for an extreme aspect ratio allocates
// levels DepthPyramid can never address and makes adopt()'s total_levels
// disagree with the CPU chain.
uint32_t GpuRenderer::pyramid_level_count(uint32_t width, uint32_t height) const
{
	uint32_t count = 1;
	for (uint32_t m = std::min(width, height); m > 1; ++count) {
		m /= 2;
	}
	return count;
}

bool GpuRenderer::create_occlusion_pipeline()
{
	SDL_GPUComputePipelineCreateInfo info{};
	info.code       = depth_comp_spv;
	info.code_size  = depth_comp_spv_len;
	info.entrypoint = "main";
	info.format     = SDL_GPU_SHADERFORMAT_SPIRV;
	// Both slots are image2D storage textures.
	info.num_readwrite_storage_textures = 2;
	info.threadcount_x                  = 8;
	info.threadcount_y                  = 8;
	info.threadcount_z                  = 1;

	hiz_pipeline_ = SDL_CreateGPUComputePipeline(device_, &info);
	return hiz_pipeline_ != nullptr;
}

void GpuRenderer::destroy_occlusion()
{
	if (hiz_pipeline_) {
		SDL_ReleaseGPUComputePipeline(device_, hiz_pipeline_);
		hiz_pipeline_ = nullptr;
	}
	if (pyramid_fence_) {
		SDL_ReleaseGPUFence(device_, pyramid_fence_);
		pyramid_fence_ = nullptr;
	}
	if (pyramid_transfer_) {
		SDL_ReleaseGPUTransferBuffer(device_, pyramid_transfer_);
		pyramid_transfer_ = nullptr;
	}
	if (pyramid_texture_) {
		SDL_ReleaseGPUTexture(device_, pyramid_texture_);
		pyramid_texture_ = nullptr;
	}
	pyramid_levels_    = 0;
	pyramid_width_     = 0;
	pyramid_height_    = 0;
	pyramid_in_flight_ = false;
	pyramid_offset_.clear();
	occlusion_pyramid_.clear();
	occlusion_ready_   = false;
}
```

- [ ] **Step 3: Implement `ensure_pyramid`**

```cpp
void GpuRenderer::ensure_pyramid(uint32_t width, uint32_t height)
{
	if (pyramid_texture_ && pyramid_width_ == width && pyramid_height_ == height) {
		return;
	}

	// Anything referencing the old texture is dropped rather than waited on.
	destroy_occlusion();

	if (width == 0 || height == 0 || !hiz_pipeline_) {
		return;
	}

	const uint32_t levels = pyramid_level_count(width, height);

	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
	                             SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
	info.width                = width;
	info.height               = height;
	info.layer_count_or_depth = 1;
	info.num_levels           = levels;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	pyramid_texture_ = SDL_CreateGPUTexture(device_, &info);
	if (!pyramid_texture_) {
		std::cerr << "occlusion: depth pyramid unavailable: " << SDL_GetError()
		          << '\n';
		return;
	}

	// Only mips from kDepthPyramidMinLevel are ever transferred.
	pyramid_offset_.assign(levels, 0u);
	uint32_t bytes = 0;
	for (uint32_t level = kDepthPyramidMinLevel; level < levels; ++level) {
		const uint32_t w = std::max(1u, width >> level);
		const uint32_t h = std::max(1u, height >> level);
		const uint32_t pitch =
		        ((w * sizeof(float) + kTransferRowAlignment - 1) /
		         kTransferRowAlignment) *
		        kTransferRowAlignment;
		pyramid_offset_[level] = bytes;
		bytes += pitch * h;
	}

	if (bytes > 0) {
		SDL_GPUTransferBufferCreateInfo tb{};
		tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		tb.size  = bytes;
		pyramid_transfer_ = SDL_CreateGPUTransferBuffer(device_, &tb);
		if (!pyramid_transfer_) {
			std::cerr << "occlusion: transfer buffer unavailable: "
			          << SDL_GetError() << '\n';
		}
	}

	pyramid_width_  = width;
	pyramid_height_ = height;
	pyramid_levels_ = levels;
}
```

- [ ] **Step 4: Implement the fence poll**

```cpp
void GpuRenderer::poll_pyramid_fence()
{
	occlusion_ready_ = false;
	if (!pyramid_in_flight_ || !pyramid_fence_) {
		return;
	}

	bool signaled = false;
	if (!SDL_QueryGPUFence(device_, pyramid_fence_, &signaled) || !signaled) {
		return; // still in flight: skip occlusion this frame, never block
	}

	SDL_ReleaseGPUFence(device_, pyramid_fence_);
	pyramid_fence_     = nullptr;
	pyramid_in_flight_ = false;

	if (!pyramid_transfer_ || pyramid_levels_ <= kDepthPyramidMinLevel) {
		return;
	}

	void* mapped = SDL_MapGPUTransferBuffer(device_, pyramid_transfer_, false);
	if (!mapped) {
		return;
	}
	const auto* bytes = static_cast<const std::uint8_t*>(mapped);

	// The GPU already reduced these levels, so install them as-is. Re-reducing
	// on the CPU here would make depth.comp pointless.
	std::vector<std::vector<float>> mips(pyramid_levels_);
	for (uint32_t level = kDepthPyramidMinLevel; level < pyramid_levels_;
	     ++level) {
		const uint32_t w = std::max(1u, pyramid_width_ >> level);
		const uint32_t h = std::max(1u, pyramid_height_ >> level);
		const uint32_t pitch =
		        ((w * sizeof(float) + kTransferRowAlignment - 1) /
		         kTransferRowAlignment) *
		        kTransferRowAlignment;

		std::vector<float>& texels = mips[level];
		texels.resize(static_cast<std::size_t>(w) * h);
		for (uint32_t y = 0; y < h; ++y) {
			const auto* row = reinterpret_cast<const float*>(
			    bytes + pyramid_offset_[level] +
			    static_cast<std::size_t>(y) * pitch);
			std::copy(row, row + w,
			          texels.begin() +
			              static_cast<std::ptrdiff_t>(
			                  static_cast<std::size_t>(y) * w));
		}
	}
	SDL_UnmapGPUTransferBuffer(device_, pyramid_transfer_);

	occlusion_pyramid_.adopt(pyramid_width_, pyramid_height_,
	                         pyramid_levels_, mips);
	occlusion_ready_ = true;
}
```

`DepthPyramid::adopt` is defined in Task 1. It installs GPU-produced mips by
level rather than reducing on the CPU, which is what keeps the compute shader
load-bearing. No staging members are required; the mip vector is local.

- [ ] **Step 5: Implement `record_pyramid`**

```cpp
void GpuRenderer::record_pyramid()
{
	if (!pyramid_texture_ || !hiz_pipeline_ || !pyramid_transfer_ || !cmd_ ||
	    pyramid_levels_ <= kDepthPyramidMinLevel) {
		return;
	}

	// One compute pass per level: SDL fixes storage bindings when a pass
	// begins, so the mip pair cannot change within one pass.
	for (uint32_t level = 1; level < pyramid_levels_; ++level) {
		SDL_GPUStorageTextureReadWriteBinding bindings[2]{};
		bindings[0].texture   = pyramid_texture_;
		bindings[0].mip_level = level - 1;
		bindings[0].layer     = 0;
		bindings[1].texture   = pyramid_texture_;
		bindings[1].mip_level = level;
		bindings[1].layer     = 0;

		SDL_GPUComputePass* compute =
		        SDL_BeginGPUComputePass(cmd_, bindings, 2, nullptr, 0, nullptr, 0);
		if (!compute) {
			return;
		}
		SDL_BindGPUComputePipeline(compute, hiz_pipeline_);

		const uint32_t groups_x = (std::max(1u, pyramid_width_ >> level) + 7) / 8;
		const uint32_t groups_y = (std::max(1u, pyramid_height_ >> level) + 7) / 8;
		SDL_DispatchGPUCompute(compute, groups_x, groups_y, 1);
		SDL_EndGPUComputePass(compute);
	}

	// Copy passes cannot overlap compute passes, so this opens only now.
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
	if (!copy) {
		return;
	}
	for (uint32_t level = kDepthPyramidMinLevel; level < pyramid_levels_; ++level) {
		const uint32_t w = std::max(1u, pyramid_width_ >> level);
		const uint32_t h = std::max(1u, pyramid_height_ >> level);

		SDL_GPUTextureRegion source{};
		source.texture  = pyramid_texture_;
		source.mip_level = level;
		source.layer    = 0;
		source.w        = w;
		source.h        = h;
		source.d        = 1;

		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = pyramid_transfer_;
		destination.offset = pyramid_offset_[level];
		// pixels_per_row IS the destination row stride, so it must be the
		// padded pitch expressed in pixels. Setting it to the raw width makes
		// the GPU write row y at y * w * 4 while the unpack in Step 4 walks
		// y * pitch, so every row after the first is garbage whenever
		// w * sizeof(float) is not a multiple of 256.
		destination.pixels_per_row = pitch / sizeof(float);
		destination.rows_per_layer = h;

		SDL_DownloadFromGPUTexture(copy, &source, &destination);
	}
	SDL_EndGPUCopyPass(copy);
}
```

- [ ] **Step 6: Hook it into the frame**

In `GpuRenderer::create_pipeline` (or `init`/`init_offscreen`), after the graphics pipeline is created:

```cpp
	occlusion_enabled_ = create_occlusion_pipeline();
	if (!occlusion_enabled_) {
		std::cerr << "occlusion: compute pipeline unavailable, "
		             "continuing without depth culling\n";
	}
```

In `init_offscreen` and anywhere `ensure_depth_target` is called, call `ensure_pyramid(width, height)` immediately after.

At the top of `begin_frame`, before anything else:

```cpp
	poll_pyramid_fence();
```

In `begin_scene_pass`, add the second color target and its clear:

```cpp
	SDL_GPUColorTargetInfo targets[2]{};
	targets[0].texture     = offscreen_ ? offscreen_color_ : swapchain_;
	targets[0].mip_level   = 0;
	targets[0].clear_color = SDL_FColor{clear_r_, clear_g_, clear_b_, 1.0f};
	targets[0].load_op     = SDL_GPU_LOADOP_CLEAR;
	targets[0].store_op    = SDL_GPU_STOREPOP_STORE;
	// Load-bearing: an unwritten texel would read as ~0 and min-reduction
	// would spread that zero across the whole pyramid, culling everything.
	targets[1].texture     = pyramid_texture_;
	targets[1].mip_level   = 0;
	targets[1].clear_color = SDL_FColor{kDepthFar, kDepthFar, kDepthFar, kDepthFar};
	targets[1].load_op     = SDL_GPU_LOADOP_CLEAR;
	targets[1].store_op    = SDL_GPU_STOREPOP_STORE;
```

Use `num_color_targets = 2` when `pyramid_texture_` is non-null, otherwise keep the current single-target path exactly as it is. Guard the whole two-target branch so a device without the pyramid still renders.

In `create_pipeline`, when the pyramid exists, declare two color targets:

```cpp
	SDL_GPUColorTargetDescription color_targets[2]{};
	color_targets[0].format = offscreen_ ? SDL_GPU_TEXTUREFORMAT_RGBA8_UNORM
	                                    : swapchain_format_;
	color_targets[1].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	pipe.target_info.color_target_descriptions     = color_targets;
	pipe.target_info.num_color_targets              = 2;
```

When the pyramid does not exist, keep the existing single-target setup unchanged.

Extend `VertexUniforms` in `src/gpu/renderer.cpp` to match the shader, adding `view` after `view_projection`:

```cpp
struct VertexUniforms {
	glm::mat4 view_projection;
	glm::mat4 view;
};
```

and upload it with both matrices in `draw`.

At the end of `end_scene_pass`, after the pass is ended, call `record_pyramid()`.

In `end_frame`, replace the submit with a fence-acquiring submit:

```cpp
	if (pyramid_in_flight_ && pyramid_fence_) {
		SDL_ReleaseGPUFence(device_, pyramid_fence_);
		pyramid_fence_ = nullptr;
	}
	pyramid_in_flight_ = false;

	if (pyramid_transfer_ && occlusion_enabled_) {
		pyramid_fence_ =
		        SDL_SubmitGPUCommandBufferAndAcquireFence(cmd_);
		if (pyramid_fence_) {
			pyramid_in_flight_ = true;
		} else {
			SDL_SubmitGPUCommandBuffer(cmd_);
		}
	} else {
		SDL_SubmitGPUCommandBuffer(cmd_);
	}
	cmd_ = nullptr;
```

If the existing `end_frame` already nulls `cmd_` or handles the return value, preserve that and only change the submit call.

No extra staging members are needed: `poll_pyramid_fence` builds its mip vector
locally and hands it straight to `DepthPyramid::adopt`.

Call `destroy_occlusion()` from `GpuRenderer::shutdown`.

- [ ] **Step 7: Build and run the CPU tests**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -20`
Expected: compiles clean.

Run: `./bin/lotus_tests 2>&1 | tail -3`
Expected: `[  PASSED  ] 131 tests.`

- [ ] **Step 8: Commit**

```bash
git add include/lotus/gpu/renderer.hpp src/gpu/renderer.cpp \
        include/lotus/scene/depth_pyramid.hpp src/scene/depth_pyramid.cpp
git commit -m "Build the Hi-Z pyramid on the GPU and read it back without blocking"
```

---

### Task 6: Demo scene, editor stats, and headless verification

**Files:**
- Modify: `app/main.cpp`
- Modify: `src/ui/editor.cpp`

**Interfaces:**
- Consumes: `Scene::occluded_entities()` from Task 3, `GpuRenderer::occlusion_ready()` and `occlusion_enabled()` from Task 5, `LOTUS_MAX_FRAMES` at `src/core/application/application.cpp:335`.
- Produces: nothing; this is the end of the plan.

- [ ] **Step 1: Add the wall to the demo scene**

In `app/main.cpp`, after the `lod_sphere` block, add:

```cpp
		// Occlusion: a wall between the camera and the LOD sphere. Once the
		// Hi-Z pyramid has a frame of depth, the sphere is culled as hidden.
		const lotus::Entity wall = scene.create("wall");
		scene.registry().emplace<lotus::MeshSource>(
		        wall, lotus::MeshSource{"builtin:cube"});
		scene.registry().emplace<lotus::Material>(
		        wall, lotus::Material{{0.55f, 0.55f, 0.58f}});
		scene.registry().get<lotus::Transform>(wall).scale = {14.0f, 14.0f, 0.25f};
		scene.registry().get<lotus::Transform>(wall).position = {0.0f, 0.0f, 2.2f};
```

Verify the wall sits between the camera at `{3.0f, 2.5f, 4.0f}` and `lod_sphere` at `{0.0f, 0.0f, -1.2f}`, and that its 0.25 depth still covers the sphere on screen. Adjust `position` if the editor shows it does not.

- [ ] **Step 2: Show the counters in the editor**

In `src/ui/editor.cpp`, replace the stats line:

```cpp
		ImGui::Text("Drawn: %u  Culled: %u  Occluded: %u",
		            scene ? scene->drawn_entities() : 0u,
		            scene ? scene->culled_entities() : 0u,
		            scene ? scene->occluded_entities() : 0u);
```

Add a line reporting readiness, using whichever renderer handle the editor already holds. If `editor.cpp` has no renderer reference, pass one through `draw` and show:

```cpp
		ImGui::Text("Occlusion: %s",
		            occlusion_ready ? "active" : "warming up");
```

- [ ] **Step 3: Add the headless GPU verification test**

`bin/lotus` opens a window and cannot run in a headless container — it fails
with `This surface does not support presenting!`. The gate is therefore a
gtest that drives `init_offscreen`, which creates a Vulkan device with no
window at all (verified working here against Mesa lavapipe).

Create `test/occlusion_gpu_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(OcclusionGpuTest, PyramidReachesTheCpuWithoutBlocking)
{
    GpuRenderer gpu;
    ASSERT_NO_THROW(gpu.init_offscreen(256, 256));
    ASSERT_TRUE(gpu.occlusion_enabled()) << "compute pipeline unavailable";

    Scene scene{"s"};
    const Entity camera = scene.create("camera");
    scene.registry().emplace<Camera3D>(camera, Camera3D{{0.0f, 0.0f, 10.0f},
                                                       {0.0f, 0.0f, 0.0f},
                                                       {0.0f, 1.0f, 0.0f},
                                                       60.0f, 0.1f, 100.0f});

    // A wall across the view, and a cube behind it.
    const Entity wall = scene.create("wall");
    scene.registry().emplace<MeshSource>(wall, MeshSource{"builtin:cube"});
    scene.registry().get<Transform>(wall).scale = {40.0f, 40.0f, 0.25f};
    scene.registry().get<Transform>(wall).position = {0.0f, 0.0f, 5.0f};

    const Entity hidden = scene.create("hidden");
    scene.registry().emplace<MeshSource>(hidden, MeshSource{"builtin:cube"});
    scene.registry().get<Transform>(hidden).position = {0.0f, 0.0f, 0.0f};

    scene.update(0.0f);

    // A few frames so the fence for an early frame lands.
    bool ready = false;
    uint32_t occluded = 0;
    for (int frame = 0; frame < 8 && !ready; ++frame) {
        gpu.begin_frame_offscreen();
        scene.upload(gpu, 1.0f);
        gpu.begin_scene_pass();
        scene.draw(gpu);
        gpu.end_scene_pass();
        gpu.end_frame();
        SDL_WaitForGPUIdle(gpu.device());
        ready = gpu.occlusion_ready();
        occluded = scene.occluded_entities();
    }

    EXPECT_TRUE(ready) << "pyramid never reached the CPU";
    EXPECT_GT(occluded, 0u) << "nothing behind the wall was culled";
}
```

- [ ] **Step 4: Run it**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -10 && ./bin/lotus_tests --gtest_filter='OcclusionGpuTest.*'`
Expected: PASS.

If it reports an SDL error about storage textures, mip levels, or the compute
pipeline, capture the exact `SDL_GetError()` string before changing anything.

If `occlusion_enabled()` is false, the compute pipeline could not be created;
that is the designed fallback, not a failure of this test — report it rather
than loosening the assertion.

- [ ] **Step 5: Run the full suite and commit**

Run: `cd build && ./bin/lotus_tests 2>&1 | tail -3`
Expected: `[  PASSED  ] 132 tests.`

```bash
git add app/main.cpp src/ui/editor.cpp test/occlusion_gpu_test.cpp
git commit -m "Add a headless GPU gate for the Hi-Z pyramid"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| Rules: never blocks | Task 5 (`poll_pyramid_fence`, `SDL_QueryGPUFence` only) |
| Rules: never throws | Task 5 (`create_occlusion_pipeline` returns bool, logs once) |
| Rules: one-directional test | Task 2 (`farthest` maximum, conservative rejections) |
| Rules: empty/missing/resized pyramid shows everything | Task 2 (`empty()` early return), Task 5 (`destroy_occlusion`) |
| Rules: `build_vertices` unchanged without a pyramid | Task 3 (default argument, nullptr test) |
| Rules: clear to far every frame | Task 4 (clear comment), Task 5 (`targets[1].clear_color`) |
| Rules: `init_offscreen` sizes the pyramid | Task 5 (`ensure_pyramid` next to `ensure_depth_target`) |
| Depth source: MRT linear depth | Task 4 |
| Depth source: why linear not `gl_FragCoord.z` | Task 4 comment, Task 3 |
| Pyramid: one compute pass per level | Task 5 (`record_pyramid` loop) |
| Pyramid: usage flags, `SIMULTANEOUS_READ_WRITE` | Task 5 (`ensure_pyramid`) |
| Readback: fence submit, poll next frame | Task 5 (`end_frame`, `begin_frame`) |
| Readback: per-mip transfer addressing, 256-byte pitch | Task 5 (`pyramid_offset_`, `kTransferRowAlignment`) |
| The test: six steps | Task 2 |
| Integration table | Tasks 3, 5, 6 |
| Failure handling table | Task 5 |
| Tests | Tasks 1, 2, 3, 6 |
| Non-goals | Respected: no per-entity occluder flags, no serialization, no LOD changes, no GPU-side sphere test |

**Placeholder scan:** clean. An earlier draft of Task 5 offered two alternative
readback routes and carried a wrong square-reconstruction block; both were
removed and replaced with a single `DepthPyramid::adopt` path. One further
placeholder was removed from Task 2 Step 1, which previously told the implementer
to write a deliberately malformed test and then replace it.

**Type consistency:** `occluded(view, view_projection, BoundingSphere)` is identical in Task 2's declaration, definition, and Task 3's call site. `kDepthFar` is `1.0e9f` in `depth_pyramid.hpp` and `1.0e9` in `depth.comp`. `kDepthPyramidMinLevel` is `2` in the header and referenced by name in Task 5. `build_vertices(float, const DepthPyramid* = nullptr)` matches across Tasks 3, 5, and 6.