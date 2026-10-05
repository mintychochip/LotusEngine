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