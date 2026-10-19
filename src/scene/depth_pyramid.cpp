#include "lotus/scene/depth_pyramid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace lotus {
namespace {

/// Halves \p value, never going below one.
uint32_t halve(uint32_t value)
{
	return value > 1 ? value / 2 : 1;
}

/// Grows the sphere so it does not occlude itself and then flicker as the
/// camera moves.
constexpr float kRadiusBias = 1.02f;

/// Clamps \p value into [lo, hi].
uint32_t clamp_index(uint32_t value, uint32_t lo, uint32_t hi)
{
	return value < lo ? lo : (value > hi ? hi : value);
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

	// The chain ends when the smaller dimension reaches 1x1, so its depth is
	// set by the smaller dimension. A non-square chain simply keeps the larger
	// dimension above 1 at the coarsest level.
	uint32_t count = 1;
	for (uint32_t m = std::min(width, height); m > 1; ++count) {
		m = halve(m);
	}

	levels_.resize(count);
	levels_[0].assign(depth, depth + static_cast<std::size_t>(width) * height);

	for (uint32_t level = 1; level < count; ++level) {
		const uint32_t src_w = level_width(level - 1);
		const uint32_t dst_w = level_width(level);
		const uint32_t dst_h = level_height(level);

		const std::vector<float>& src = levels_[level - 1];
		std::vector<float>& dst      = levels_[level];
		dst.assign(static_cast<std::size_t>(dst_w) * dst_h, kDepthFar);

		for (uint32_t y = 0; y < dst_h; ++y) {
			for (uint32_t x = 0; x < dst_w; ++x) {
				// Mip dimensions follow Vulkan's max(1, w >> level)
				// convention, exactly as shaders/depth.comp does. With that
				// rule, dst_w = src_w >> 1, so every 2x2 window lies wholly
				// inside the source and a texel in a ragged edge column or
				// row simply falls outside all of them and is never sampled.
				float nearest = kDepthFar;
				for (uint32_t dy = 0; dy < 2; ++dy) {
					for (uint32_t dx = 0; dx < 2; ++dx) {
						const std::size_t sx = x * 2 + dx;
						const std::size_t sy = y * 2 + dy;
						nearest = std::min(nearest, src[sy * src_w + sx]);
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
	// A rejected upload must leave no pyramid at all, never the previous
	// frame's chain paired with this frame's camera.
	clear();
	if (width == 0 || height == 0 || total_levels == 0) {
		return;
	}
	if (mips.size() < total_levels) {
		return; // refuse a short upload rather than query half a chain
	}

	width_  = width;
	height_ = height;

	// Every transferred mip must match the dimensions its accessors report, or
	// a consumer indexing by level_width/level_height would run off the end.
	// Levels below kDepthPyramidMinLevel are legitimately empty and skipped.
	for (uint32_t level = kDepthPyramidMinLevel; level < total_levels; ++level) {
		const std::size_t expected = static_cast<std::size_t>(level_width(level)) *
		                             level_height(level);
		if (mips[level].size() != expected) {
			clear();
			return;
		}
	}

	levels_.assign(total_levels, {});
	for (uint32_t level = kDepthPyramidMinLevel; level < total_levels; ++level) {
		levels_[level] = mips[level];
	}
}

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

	// Project the eight corners of the sphere's world-space AABB. The corners
	// must be built in WORLD space: view_projection already contains view, so
	// handing it a view-space point applies the view transform twice and halves
	// every footprint. A world-space AABB is larger than the sphere for a
	// rotated camera, which is the conservative direction.
	glm::vec2 screen_min{std::numeric_limits<float>::max(),
	                    std::numeric_limits<float>::max()};
	glm::vec2 screen_max{std::numeric_limits<float>::lowest(),
	                    std::numeric_limits<float>::lowest()};
	for (int corner = 0; corner < 8; ++corner) {
		const glm::vec3 offset{(corner & 1) ? radius : -radius,
		                       (corner & 2) ? radius : -radius,
		                       (corner & 4) ? radius : -radius};
		const glm::vec4 clip =
		        view_projection * glm::vec4{world_sphere.center + offset, 1.0f};
		if (clip.w <= 0.0f) {
			return false;
		}
		// Not clip.xyz / clip.w: the bundled GLM predates that swizzle.
		const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
		screen_min = glm::min(screen_min, ndc);
		screen_max = glm::max(screen_max, ndc);
	}

	// NDC to pixels.
	const float width_f  = static_cast<float>(width_);
	const float height_f = static_cast<float>(height_);
	const float min_x = (screen_min.x * 0.5f + 0.5f) * width_f;
	const float max_x = (screen_max.x * 0.5f + 0.5f) * width_f;
	// Y is flipped. SDL's Vulkan backend sets a negative viewport height
	// "for consistency with other backends" (SDL_gpu_vulkan.c:7366-7368), so
	// ndc.y = +1 lands in framebuffer row 0, not row height-1. The pyramid is
	// built from, and downloaded in, framebuffer order, so the conversion must
	// flip too. Flipping also swaps the extremes, hence max feeds min_y.
	const float min_y = (1.0f - (screen_max.y * 0.5f + 0.5f)) * height_f;
	const float max_y = (1.0f - (screen_min.y * 0.5f + 0.5f)) * height_f;

	// Outside the viewport: leave it to the frustum test.
	if (max_x < 0.0f || min_x > width_f || max_y < 0.0f || min_y > height_f) {
		return false;
	}

	const float clamped_min_x = std::max(0.0f, min_x);
	const float clamped_max_x = std::min(width_f, max_x);
	const float clamped_min_y = std::max(0.0f, min_y);
	const float clamped_max_y = std::min(height_f, max_y);

	// Read the finest mip the chain actually holds, then take the maximum over
	// the footprint. Coarser mips would be cheaper, but this pyramid is
	// min-reduced: a coarse texel is the NEAREST occluder anywhere in its
	// block, so taking the max over coarse texels smears one small occluder
	// across a whole block and reports the far, empty parts of the footprint as
	// covered. That culls spheres that are genuinely visible. Soundness wins
	// over the handful of texels this costs.
	const uint32_t last = count - 1;
	const uint32_t level = clamp_index(kDepthPyramidMinLevel, 0, last);

	// adopt() accepts chains shorter than kDepthPyramidMinLevel, leaving those
	// levels empty. Reading one would run off the end, so treat it as "no
	// depth recorded" and keep the sphere.
	if (levels_[level].empty()) {
		return false;
	}

	const uint32_t mip_w = level_width(level);
	const uint32_t mip_h = level_height(level);

	// A texel at this level always covers exactly 2^kDepthPyramidMinLevel
	// level-0 texels on each axis; only the texel COUNT is ragged. Scaling by
	// block size, and the trailing ragged column would be judged by its
	// left-hand neighbour.
	const float block = static_cast<float>(1u << kDepthPyramidMinLevel);
	const float span_x = block * static_cast<float>(mip_w);
	const float span_y = block * static_cast<float>(mip_h);

	// Pixels beyond the representable span have no texel at this level, so
	// there is no data to prove occlusion. Assume visible.
	if (clamped_max_x > span_x || clamped_max_y > span_y) {
		return false;
	}

	uint32_t x0 = static_cast<uint32_t>(std::floor(clamped_min_x / block));
	uint32_t y0 = static_cast<uint32_t>(std::floor(clamped_min_y / block));
	uint32_t x1 = static_cast<uint32_t>(
	    std::min(span_x, std::ceil(clamped_max_x / block)));
	uint32_t y1 = static_cast<uint32_t>(
	    std::min(span_y, std::ceil(clamped_max_y / block)));

	x0 = std::min(x0, mip_w - 1);
	y0 = std::min(y0, mip_h - 1);
	x1 = std::max(x1, x0 + 1);
	y1 = std::max(y1, y0 + 1);

	// The farthest occluder anywhere under the footprint, as far as this
	// min-reduced mip can tell. A texel summarises its block by taking the
	// NEAREST value in it, so `farthest` is a lower bound on the footprint's
	// true maximum, not an upper one -- taking the maximum alone is therefore
	// NOT what makes the test conservative. What makes it conservative is the
	// kDepthFar veto below: one footprint texel reading "nothing here" drives
	// `farthest` to kDepthFar and rejects the cull.
	//
	// Accepted residual limitation: a texel summarising a block can still
	// lose a sphere that is only partly covered by that block's occluder.
	//
	// Seeded with the lowest float, not kDepthFar: kDepthFar is the "nothing
	// here" sentinel and is the largest value a texel can hold, so seeding
	// with it would pin `farthest` there and never cull anything.
	const std::vector<float>& mip = levels_[level];
	float farthest = std::numeric_limits<float>::lowest();
	for (uint32_t y = y0; y < y1; ++y) {
		const std::size_t row = static_cast<std::size_t>(y) * mip_w;
		for (uint32_t x = x0; x < x1; ++x) {
			farthest = std::max(farthest, mip[row + x]);
			if (farthest >= kDepthFar) {
				return false; // nothing there: visible, and stop scanning
			}
		}
	}

	return nearest_depth >= farthest;
}

} // namespace lotus
