#pragma once

#include <cstdint>

#include "gpu/vk/vk_api.hpp"

namespace lotus::vk {

/// Shader module from a committed SPIR-V blob (`include/lotus/gpu/*.spv.h`).
VkShaderModule create_shader_module(const Api& api, VkDevice device,
                                    const unsigned char* bytes,
                                    unsigned long long size);

/// The vertex input the lit and shadow pipelines share: slot 0 is a mesh vertex
/// (`GpuVertex`, 48 B), slot 1 its UV (8 B), slot 2 one `GpuInstance` (96 B) at
/// instance rate. Attributes land at locations 0..13 exactly as lit.vert and
/// shadow.vert declare them; the fullscreen passes (sky, FXAA, SSAO) bind
/// nothing.
void lit_vertex_input(VkVertexInputBindingDescription (&bindings)[3],
                      VkVertexInputAttributeDescription (&attributes)[11]);

struct GraphicsPipelineInfo {
	VkPipelineLayout layout{VK_NULL_HANDLE};
	VkShaderModule vertex{VK_NULL_HANDLE};
	VkShaderModule fragment{VK_NULL_HANDLE};
	uint32_t color_target_count{0};
	VkFormat color_formats[2]{VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
	/// Zeroes target 1's write mask (the sky leaves the linear-depth target
	/// alone; its value is what the pass cleared it to). Needs independentBlend.
	bool mask_target1{false};
	VkFormat depth_format{VK_FORMAT_UNDEFINED}; ///< VK_FORMAT_UNDEFINED = no depth
	bool depth_test{true};
	bool depth_write{true};
	VkCompareOp depth_compare{VK_COMPARE_OP_LESS_OR_EQUAL};
	bool vertex_input{false};
	/// Depth clamp rather than clip: geometry past the far plane is drawn, at
	/// the top of the viewport's depth range, instead of being dropped. The
	/// engine's sky-versus-geometry depth convention depends on it, and so does
	/// the benchmark scene's ground plane.
	bool depth_clamp{true};
};

/// A graphics pipeline for dynamic rendering (no VkRenderPass, no framebuffer):
/// the formats in \p info describe the render targets it will be used with.
VkPipeline create_graphics_pipeline(const Api& api, VkDevice device,
                                    const GraphicsPipelineInfo& info,
                                    const char* name);

/// The Hi-Z reduction: depth.comp, 8x8x1 workgroups, no push constants.
VkPipeline create_compute_pipeline(const Api& api, VkDevice device,
                                   VkPipelineLayout layout, VkShaderModule module,
                                   const char* name);

/// Pipeline layout over \p sets, in order, with no push constants: every uniform
/// block is a dynamic-offset UBO descriptor, which is what the shaders were
/// compiled against.
VkPipelineLayout create_pipeline_layout(const Api& api, VkDevice device,
                                        const VkDescriptorSetLayout* sets,
                                        uint32_t set_count);

} // namespace lotus::vk