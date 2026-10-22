#include "gpu/vk/vk_pipelines.hpp"

#include <array>

namespace lotus::vk {
namespace {

VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits stage,
                                      VkShaderModule module)
{
	VkPipelineShaderStageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage  = stage;
	info.module = module;
	info.pName  = "main";
	return info;
}

} // namespace

VkShaderModule create_shader_module(const Api& api, VkDevice device,
                                    const unsigned char* bytes,
                                    unsigned long long size)
{
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = static_cast<std::size_t>(size);
	info.pCode    = reinterpret_cast<const std::uint32_t*>(bytes);
	VkShaderModule module{VK_NULL_HANDLE};
	check(api.CreateShaderModule(device, &info, nullptr, &module),
	      "vkCreateShaderModule");
	return module;
}

void lit_vertex_input(VkVertexInputBindingDescription (&bindings)[3],
                      VkVertexInputAttributeDescription (&attributes)[11])
{
	bindings[0].binding   = 0;
	bindings[0].stride    = 48; // sizeof(GpuVertex)
	bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	bindings[1].binding   = 1;
	bindings[1].stride    = 8; // glm::vec2
	bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	bindings[2].binding   = 2;
	bindings[2].stride    = 96; // sizeof(GpuInstance)
	bindings[2].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

	const auto attribute = [](uint32_t location, uint32_t binding,
	                          VkFormat format, uint32_t offset) {
		VkVertexInputAttributeDescription description{};
		description.location = location;
		description.binding  = binding;
		description.format   = format;
		description.offset   = offset;
		return description;
	};
	attributes[0] = attribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);   // position
	attributes[1] = attribute(1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16);  // normal
	attributes[2] = attribute(2, 0, VK_FORMAT_R32G32B32_SFLOAT, 32);  // colour
	attributes[3] = attribute(4, 1, VK_FORMAT_R32G32_SFLOAT, 0);      // uv
	attributes[4] = attribute(6, 0, VK_FORMAT_R32_SFLOAT, 44);        // roughness
	for (uint32_t column = 0; column < 4; ++column) {                 // model mat4
		attributes[5 + column] =
		        attribute(8 + column, 2, VK_FORMAT_R32G32B32A32_SFLOAT,
		                  column * 16);
	}
	attributes[9]  = attribute(12, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 64); // shade
	attributes[10] = attribute(13, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 80); // material
}

VkPipeline create_graphics_pipeline(const Api& api, VkDevice device,
                                    const GraphicsPipelineInfo& info,
                                    const char* /*name*/)
{
	const VkPipelineShaderStageCreateInfo stages[2] = {
	        stage(VK_SHADER_STAGE_VERTEX_BIT, info.vertex),
	        stage(VK_SHADER_STAGE_FRAGMENT_BIT, info.fragment)};

	VkVertexInputBindingDescription bindings[3]{};
	VkVertexInputAttributeDescription attributes[11]{};
	VkPipelineVertexInputStateCreateInfo vertex_input{};
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	if (info.vertex_input) {
		lit_vertex_input(bindings, attributes);
		vertex_input.vertexBindingDescriptionCount   = 3;
		vertex_input.pVertexBindingDescriptions      = bindings;
		vertex_input.vertexAttributeDescriptionCount = 11;
		vertex_input.pVertexAttributeDescriptions    = attributes;
	}

	VkPipelineInputAssemblyStateCreateInfo assembly{};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewport{};
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	// Depth is clamped, never clipped: geometry beyond the far plane has to
	// survive as its clamped depth, which is what the sky-last convention and
	// the shadow casters rely on.
	rasterizer.depthClampEnable        = info.depth_clamp ? VK_TRUE : VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode                = VK_CULL_MODE_NONE;
	rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.lineWidth               = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth{};
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	if (info.depth_format != VK_FORMAT_UNDEFINED) {
		depth.depthTestEnable  = info.depth_test ? VK_TRUE : VK_FALSE;
		depth.depthWriteEnable = info.depth_write ? VK_TRUE : VK_FALSE;
		depth.depthCompareOp   = info.depth_compare;
	}

	std::array<VkPipelineColorBlendAttachmentState, 2> blend{};
	for (uint32_t target = 0; target < info.color_target_count; ++target) {
		blend[target].blendEnable = VK_FALSE;
		blend[target].colorWriteMask =
		        (target == 1 && info.mask_target1)
		                ? 0
		                : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	}
	VkPipelineColorBlendStateCreateInfo color{};
	color.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color.attachmentCount = info.color_target_count;
	color.pAttachments    = blend.data();

	const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
	                                          VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic{};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates    = dynamic_states;

	VkPipelineRenderingCreateInfo rendering{};
	rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount    = info.color_target_count;
	rendering.pColorAttachmentFormats = info.color_formats;
	rendering.depthAttachmentFormat   = info.depth_format;

	VkGraphicsPipelineCreateInfo create{};
	create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create.pNext               = &rendering;
	create.stageCount          = 2;
	create.pStages             = stages;
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterizer;
	create.pMultisampleState   = &multisample;
	create.pDepthStencilState  = &depth;
	create.pColorBlendState    = &color;
	create.pDynamicState       = &dynamic;
	create.layout              = info.layout;
	create.renderPass          = VK_NULL_HANDLE; // dynamic rendering

	VkPipeline pipeline{VK_NULL_HANDLE};
	check(api.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create, nullptr,
	                                  &pipeline),
	      "vkCreateGraphicsPipelines");
	return pipeline;
}

VkPipeline create_compute_pipeline(const Api& api, VkDevice device,
                                   VkPipelineLayout layout, VkShaderModule module,
                                   const char* /*name*/)
{
	VkComputePipelineCreateInfo create{};
	create.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	create.stage  = stage(VK_SHADER_STAGE_COMPUTE_BIT, module);
	create.layout = layout;
	VkPipeline pipeline{VK_NULL_HANDLE};
	check(api.CreateComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr,
	                                 &pipeline),
	      "vkCreateComputePipelines");
	return pipeline;
}

VkPipelineLayout create_pipeline_layout(const Api& api, VkDevice device,
                                        const VkDescriptorSetLayout* sets,
                                        uint32_t set_count)
{
	VkPipelineLayoutCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	info.setLayoutCount = set_count;
	info.pSetLayouts    = sets;
	VkPipelineLayout layout{VK_NULL_HANDLE};
	check(api.CreatePipelineLayout(device, &info, nullptr, &layout),
	      "vkCreatePipelineLayout");
	return layout;
}

} // namespace lotus::vk