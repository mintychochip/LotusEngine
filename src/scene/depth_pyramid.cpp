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
		const uint32_t src_h = level_height(level - 1);
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

} // namespace lotus
