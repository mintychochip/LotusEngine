// Descriptor layouts, pipelines and the shadow/lit/sky passes.

#include "gpu/vulkan/renderer_vulkan.hpp"

#include "lotus/scene/assets.hpp"

#include "lotus/gpu/lit.frag.spv.h"
#include "lotus/gpu/lit.vert.spv.h"
#include "lotus/gpu/shadow.frag.spv.h"
#include "lotus/gpu/shadow.vert.spv.h"
#include "lotus/gpu/sky.frag.spv.h"
#include "lotus/gpu/sky.vert.spv.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace lotus {
namespace {

/// Geometry in the lit pass is rasterised into the lower half of the depth
/// range, [0, kGeometryDepthMax], through the viewport; the sky is drawn last,
/// at depth 1.0, into the half no geometry reaches. See the reference for why
/// a power of two keeps the picture pixel for pixel.
constexpr float kGeometryDepthMax = 0.5f;

/// 1024px D16 depth rendered from the light each frame, sampled by lit.frag
/// with a 3x3 PCF. D16 (not D32) because it is the one depth format
/// universally sampler-supported.
constexpr uint32_t kShadowMapSize        = 1024;
constexpr VkFormat kShadowFormat         = VK_FORMAT_D16_UNORM;
constexpr float kShadowBias              = 0.0015f;
constexpr float kShadowNormalOffset      = 0.05f;
constexpr float kShadowOrthoHalf         = 15.0f;
constexpr float kShadowLightDistance     = 30.0f;
constexpr float kShadowNear              = 1.0f;
constexpr float kShadowFar               = 60.0f;

constexpr VkFormat kColorFormat   = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat   = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kPyramidFormat = VK_FORMAT_R32_SFLOAT;

VkDescriptorSetLayoutBinding binding(uint32_t index,
                                     VkDescriptorType type,
                                     VkShaderStageFlags stages)
{
	VkDescriptorSetLayoutBinding out{};
	out.binding            = index;
	out.descriptorType     = type;
	out.descriptorCount    = 1;
	out.stageFlags         = stages;
	out.pImmutableSamplers = nullptr;
	return out;
}

/// The shadow depth pass: no colour targets, D16 depth, and only the vertex
/// input shadow.vert consumes (position at location 0, the model matrix at
/// 8..11). Every other state matches the lit pipeline: depth clamp on (far
/// geometry must survive clamped, the shadow casters rely on it), no culling,
/// LESS_OR_EQUAL, dynamic viewport/scissor, dynamic rendering.
VkPipeline create_shadow_pipeline(const vk::Api& api, VkDevice device,
                                  VkPipelineLayout layout,
                                  VkShaderModule vertex, VkShaderModule fragment)
{
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName  = "main";

	VkVertexInputBindingDescription bindings[2]{};
	bindings[0].binding   = 0;
	bindings[0].stride    = 48; // sizeof(GpuVertex)
	bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	bindings[1].binding   = 2;
	bindings[1].stride    = 80; // sizeof(GpuInstance)
	bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
	VkVertexInputAttributeDescription attributes[5]{};
	attributes[0].location = 0;
	attributes[0].binding  = 0;
	attributes[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
	attributes[0].offset   = 0;
	for (uint32_t column = 0; column < 4; ++column) {
		attributes[1 + column].location = 8 + column;
		attributes[1 + column].binding  = 2;
		attributes[1 + column].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
		attributes[1 + column].offset   = column * 16;
	}
	VkPipelineVertexInputStateCreateInfo vertex_input{};
	vertex_input.sType =
	        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount   = 2;
	vertex_input.pVertexBindingDescriptions      = bindings;
	vertex_input.vertexAttributeDescriptionCount = 5;
	vertex_input.pVertexAttributeDescriptions    = attributes;

	VkPipelineInputAssemblyStateCreateInfo assembly{};
	assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewport{};
	viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable        = VK_TRUE;
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
	depth.depthTestEnable  = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

	VkPipelineColorBlendStateCreateInfo color{};
	color.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

	const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
	                                          VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic{};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates    = dynamic_states;

	VkPipelineRenderingCreateInfo rendering{};
	rendering.sType                 = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount  = 0;
	rendering.depthAttachmentFormat = VK_FORMAT_D16_UNORM;

	VkGraphicsPipelineCreateInfo create{};
	create.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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
	create.layout              = layout;
	create.renderPass          = VK_NULL_HANDLE;

	VkPipeline pipeline{VK_NULL_HANDLE};
	vk::check(api.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &create,
	                                      nullptr, &pipeline),
	          "vkCreateGraphicsPipelines");
	return pipeline;
}

} // namespace

void DescriptorLayouts::create(const vk::Context& context)
{
	const vk::Api& api    = context.api;
	const VkDevice device = context.device;
	const auto make = [&](const VkDescriptorSetLayoutBinding* bindings,
	                      uint32_t count) {
		VkDescriptorSetLayoutCreateInfo info{};
		info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount = count;
		info.pBindings    = bindings;
		VkDescriptorSetLayout layout{VK_NULL_HANDLE};
		vk::check(api.CreateDescriptorSetLayout(device, &info, nullptr,
		                                        &layout),
		          "vkCreateDescriptorSetLayout");
		return layout;
	};
	empty = make(nullptr, 0);
	const auto ubo_vertex = binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
	                                VK_SHADER_STAGE_VERTEX_BIT);
	vertex = make(&ubo_vertex, 1);
	const VkDescriptorSetLayoutBinding sampler_bindings[3] = {
	        binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	                VK_SHADER_STAGE_FRAGMENT_BIT),
	        binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	                VK_SHADER_STAGE_FRAGMENT_BIT),
	        binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	                VK_SHADER_STAGE_FRAGMENT_BIT),
	};
	samplers = make(sampler_bindings, 3);
	const VkDescriptorSetLayoutBinding storage_bindings[2] = {
	        binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	                VK_SHADER_STAGE_COMPUTE_BIT),
	        binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	                VK_SHADER_STAGE_COMPUTE_BIT),
	};
	storage = make(storage_bindings, 2);
	const auto ubo_fragment =
	        binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
	                VK_SHADER_STAGE_FRAGMENT_BIT);
	fragment = make(&ubo_fragment, 1);
}

void DescriptorLayouts::destroy(const vk::Context& context)
{
	const vk::Api& api    = context.api;
	const VkDevice device = context.device;
	for (VkDescriptorSetLayout* layout :
	     {&empty, &vertex, &samplers, &fragment, &storage}) {
		if (*layout != VK_NULL_HANDLE) {
			api.DestroyDescriptorSetLayout(device, *layout, nullptr);
			*layout = VK_NULL_HANDLE;
		}
	}
}

void GpuRendererVulkan::create_pipelines()
{
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	VkShaderModule lit_vert = vk::create_shader_module(
	        api, device, lit_vert_spv, sizeof(lit_vert_spv));
	VkShaderModule lit_frag = vk::create_shader_module(
	        api, device, lit_frag_spv, sizeof(lit_frag_spv));
	VkShaderModule shadow_vert = vk::create_shader_module(
	        api, device, shadow_vert_spv, sizeof(shadow_vert_spv));
	VkShaderModule shadow_frag = vk::create_shader_module(
	        api, device, shadow_frag_spv, sizeof(shadow_frag_spv));

	const VkDescriptorSetLayout lit_sets[4] = {
	        layouts_.empty, layouts_.vertex, layouts_.samplers,
	        layouts_.fragment};
	lit_layout_ = vk::create_pipeline_layout(api, device, lit_sets, 4);
	const VkDescriptorSetLayout shadow_sets[2] = {layouts_.empty,
	                                              layouts_.vertex};
	shadow_layout_ = vk::create_pipeline_layout(api, device, shadow_sets, 2);
	// The sky reads the same sets as the lit pass (vertex UBO at set 1,
	// fragment UBO at set 3); set 2 stays bound but unread.
	sky_layout_ = vk::create_pipeline_layout(api, device, lit_sets, 4);

	vk::GraphicsPipelineInfo lit{};
	lit.layout              = lit_layout_;
	lit.vertex              = lit_vert;
	lit.fragment            = lit_frag;
	lit.color_target_count  = 2;
	lit.color_formats[0]    = kColorFormat;
	lit.color_formats[1]    = kPyramidFormat;
	lit.mask_target1        = false;
	lit.depth_format        = kDepthFormat;
	lit.depth_test          = true;
	lit.depth_write         = true;
	lit.depth_compare       = VK_COMPARE_OP_LESS_OR_EQUAL;
	lit.vertex_input        = true;
	lit.depth_clamp         = true;
	lit_pipeline_           = vk::create_graphics_pipeline(
            api, device, lit, "lit");

	// The shadow vertex shader consumes only the position and the model
	// matrix, so its pipeline declares only those attributes: every
	// declared attribute the shader does not consume is a validation
	// warning, and this build runs validation-clean. The same buffers
	// bind here (bindings 0 and 2, same strides); binding 1 is simply
	// never consumed.
	shadow_pipeline_ = create_shadow_pipeline(api, device, shadow_layout_,
	                                          shadow_vert, shadow_frag);

	api.DestroyShaderModule(device, shadow_frag, nullptr);
	api.DestroyShaderModule(device, shadow_vert, nullptr);
	api.DestroyShaderModule(device, lit_frag, nullptr);
	api.DestroyShaderModule(device, lit_vert, nullptr);
}

VkDescriptorSet GpuRendererVulkan::alloc_vertex_set(uint64_t offset)
{
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = slot_->descriptors;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &layouts_.vertex;
	VkDescriptorSet set{VK_NULL_HANDLE};
	vk::check(context_.api.AllocateDescriptorSets(context_.device, &alloc,
	                                              &set),
	          "vkAllocateDescriptorSets");
	VkDescriptorBufferInfo buffer{};
	buffer.buffer = slot_->uniforms.handle;
	buffer.offset = 0;
	buffer.range  = sizeof(VertexUniforms);
	VkWriteDescriptorSet write{};
	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet          = set;
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	write.pBufferInfo     = &buffer;
	context_.api.UpdateDescriptorSets(context_.device, 1, &write, 0, nullptr);
	(void)offset;
	return set;
}

VkDescriptorSet GpuRendererVulkan::alloc_fragment_set(uint64_t offset)
{
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = slot_->descriptors;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &layouts_.fragment;
	VkDescriptorSet set{VK_NULL_HANDLE};
	vk::check(context_.api.AllocateDescriptorSets(context_.device, &alloc,
	                                              &set),
	          "vkAllocateDescriptorSets");
	VkDescriptorBufferInfo buffer{};
	buffer.buffer = slot_->uniforms.handle;
	buffer.offset = 0;
	buffer.range  = sizeof(FragmentUniforms);
	VkWriteDescriptorSet write{};
	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet          = set;
	write.dstBinding      = 0;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	write.pBufferInfo     = &buffer;
	context_.api.UpdateDescriptorSets(context_.device, 1, &write, 0, nullptr);
	(void)offset;
	return set;
}

VkDescriptorSet GpuRendererVulkan::alloc_sampler_set(
        VkImageView albedo, VkSampler albedo_sampler, VkImageView shadow,
        VkSampler shadow_sampler, VkImageView ao, VkSampler ao_sampler)
{
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = slot_->descriptors;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &layouts_.samplers;
	VkDescriptorSet set{VK_NULL_HANDLE};
	vk::check(context_.api.AllocateDescriptorSets(context_.device, &alloc,
	                                              &set),
	          "vkAllocateDescriptorSets");
	const VkDescriptorImageInfo images[3] = {
	        {albedo_sampler, albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	        {shadow_sampler, shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	        {ao_sampler, ao, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	};
	VkWriteDescriptorSet writes[3]{};
	for (uint32_t i = 0; i < 3; ++i) {
		writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet          = set;
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo      = &images[i];
	}
	context_.api.UpdateDescriptorSets(context_.device, 3, writes, 0, nullptr);
	return set;
}

void GpuRendererVulkan::shadow_light_matrices(glm::mat4& view, glm::mat4& vp) const
{
	glm::vec3 direction = glm::length(lighting_.direction) > 0.0f
	                            ? glm::normalize(lighting_.direction)
	                            : glm::vec3{0.0f, 1.0f, 0.0f};
	const glm::vec3 center{0.0f};
	const glm::vec3 eye = center + direction * kShadowLightDistance;
	const glm::vec3 up =
	        std::abs(direction.y) > 0.99f ? glm::vec3{1.0f, 0.0f, 0.0f}
	                                      : glm::vec3{0.0f, 1.0f, 0.0f};
	view = glm::lookAtRH(eye, center, up);
	vp   = glm::orthoRH_ZO(-kShadowOrthoHalf, kShadowOrthoHalf,
	                       -kShadowOrthoHalf, kShadowOrthoHalf, kShadowNear,
	                       kShadowFar) *
	       view;
}

bool GpuRendererVulkan::shadows_ready()
{
	if (!lighting_.shadows_enabled || !device_valid_) {
		return false;
	}
	if (!shadow_support_queried_) {
		shadow_support_queried_ = true;
		VkFormatProperties properties{};
		context_.api.GetPhysicalDeviceFormatProperties(
		        context_.physical_device, kShadowFormat, &properties);
		shadows_supported_ =
		        (properties.optimalTilingFeatures &
		         (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
		          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ==
		        (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
		         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
		if (!shadows_supported_) {
			std::cerr << "shadows: depth-sampler format unsupported, "
			             "continuing without shadow mapping\n";
		}
	}
	if (!shadows_supported_) {
		return false;
	}
	ensure_shadow_resources();
	return shadows_supported_;
}

void GpuRendererVulkan::ensure_shadow_resources()
{
	if (!device_valid_ || !shadows_supported_) {
		return;
	}
	const auto disable = [this](const char* what) {
		std::cerr << "shadows: " << what
		          << ", continuing without shadow mapping\n";
		destroy_shadows();
		shadows_supported_ = false;
	};
	if (shadow_map_.handle == VK_NULL_HANDLE) {
		shadow_map_ = create_target_image(
		        "shadow", kShadowFormat, kShadowMapSize, kShadowMapSize, 1,
		        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		                VK_IMAGE_USAGE_SAMPLED_BIT);
		if (shadow_map_.handle == VK_NULL_HANDLE) {
			disable("shadow map unavailable");
			return;
		}
		shadow_map_state_ = vk::ImageState{};
	}
	if (shadow_sampler_ == VK_NULL_HANDLE) {
		disable("shadow sampler unavailable");
		return;
	}
	if (shadow_pipeline_ == VK_NULL_HANDLE) {
		// Created at init; reaching here means creation threw, which
		// cannot happen silently, so this only guards the moved value.
		disable("shadow pipeline unavailable");
		return;
	}
}

void GpuRendererVulkan::destroy_shadows()
{
	if (shadow_map_.handle != VK_NULL_HANDLE) {
		memory_.destroy_image(shadow_map_);
		shadow_map_ = vk::Image{};
	}
	shadow_map_state_ = vk::ImageState{};
	shadow_rendered_  = false;
	light_vp_         = glm::mat4{1.0f};
}

void GpuRendererVulkan::render_shadow_map()
{
	if (shadow_rendered_ || cmd_ == VK_NULL_HANDLE || slot_ == nullptr ||
	    geometry_draws_.empty()) {
		return;
	}
	if (!shadows_ready()) {
		return;
	}
	glm::mat4 light_view{1.0f};
	shadow_light_matrices(light_view, light_vp_);

	image_as_depth_attachment(*barriers_, cmd_, shadow_map_, shadow_map_state_);
	barriers_->flush(cmd_);

	VkRenderingAttachmentInfo depth{};
	depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	depth.imageView   = shadow_map_.view;
	depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depth.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	depth.clearValue.depthStencil = VkClearDepthStencilValue{1.0f, 0};
	VkRenderingInfo rendering{};
	rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea            = VkRect2D{{0, 0},
	                                           {kShadowMapSize, kShadowMapSize}};
	rendering.layerCount            = 1;
	rendering.colorAttachmentCount  = 0;
	rendering.pDepthAttachment      = &depth;
	context_.api.CmdBeginRendering(cmd_, &rendering);

	VertexUniforms uniforms{};
	uniforms.view_projection = light_vp_;
	uniforms.view            = light_view;
	const uint64_t offset    = push_uniforms(&uniforms, sizeof(uniforms));
	VkDescriptorSet set      = alloc_vertex_set(offset);
	const uint32_t dynamic   = static_cast<uint32_t>(offset);
	set_viewport(cmd_, kShadowMapSize, kShadowMapSize, 1.0f);
	set_scissor(cmd_, kShadowMapSize, kShadowMapSize);
	context_.api.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             shadow_pipeline_);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   shadow_layout_, 1, 1, &set, 1,
	                                   &dynamic);
	for (const GeometryDraw& draw : geometry_draws_) {
		record_geometry_draw(cmd_, draw);
	}
	context_.api.CmdEndRendering(cmd_);
	image_shader_read(*barriers_, cmd_, shadow_map_, 0, 1, shadow_map_state_);
	barriers_->flush(cmd_);
	shadow_rendered_ = true;
}

void GpuRendererVulkan::bind_lit_state(const glm::mat4& view,
                                       const glm::mat4& view_projection,
                                       const glm::vec3& camera_position)
{
	VertexUniforms vertex_uniforms{};
	vertex_uniforms.view_projection = view_projection;
	vertex_uniforms.view            = view;

	FragmentUniforms fragment_uniforms{};
	const glm::vec3 direction = glm::length(lighting_.direction) > 0.0f
	                                    ? glm::normalize(lighting_.direction)
	                                    : glm::vec3{0.0f, 0.0f, 1.0f};
	fragment_uniforms.light_direction = glm::vec4{direction, 0.0f};
	fragment_uniforms.light_color =
	        glm::vec4{lighting_.color, lighting_.intensity};
	fragment_uniforms.ambient_color   = glm::vec4{lighting_.ambient, 0.0f};
	fragment_uniforms.camera_position = glm::vec4{camera_position, 1.0f};
	fragment_uniforms.tone_params =
	        glm::vec4{lighting_.exposure, 0.0f, 0.0f, 0.0f};
	fragment_uniforms.hemi_sky    = glm::vec4{lighting_.sky_color, 0.0f};
	fragment_uniforms.hemi_ground = glm::vec4{lighting_.ground_color, 0.0f};
	// Lit only when the toggle is on AND the map actually rendered this
	// frame; every other combination reads shadow_params.x == 0 and takes
	// the exact legacy unshadowed path in the shader.
	const bool shadow_on = lighting_.shadows_enabled && shadows_supported_ &&
	                       shadow_rendered_ &&
	                       shadow_map_.handle != VK_NULL_HANDLE &&
	                       shadow_sampler_ != VK_NULL_HANDLE;
	fragment_uniforms.shadow_params =
	        glm::vec4{shadow_on ? 1.0f : 0.0f, kShadowBias,
	                  kShadowNormalOffset, 1.0f / kShadowMapSize};
	fragment_uniforms.light_vp = light_vp_;
	// Fog: linear-space exp fog tinted by the sky/sun hues, never hardcoded.
	// Disabled restores the exact legacy path.
	const glm::vec3 sky_lin{pbr::srgb_to_linear(lighting_.sky_color.x),
	                        pbr::srgb_to_linear(lighting_.sky_color.y),
	                        pbr::srgb_to_linear(lighting_.sky_color.z)};
	const glm::vec3 sun_lin{pbr::srgb_to_linear(lighting_.color.x),
	                        pbr::srgb_to_linear(lighting_.color.y),
	                        pbr::srgb_to_linear(lighting_.color.z)};
	const float warmth =
	        std::min(std::max(lighting_.horizon_warmth, 0.0f), 1.0f);
	const glm::vec3 fog_color =
	        sky_lin + (sun_lin - sky_lin) * (0.25f * warmth);
	fragment_uniforms.fog_params =
	        glm::vec4{lighting_.fog_enabled ? 1.0f : 0.0f,
	                  std::max(lighting_.fog_density, 0.0f), 0.0f, 0.0f};
	fragment_uniforms.fog_color = glm::vec4{fog_color, 0.0f};
	// Bounce fill: warm third directional, unshadowed like the hemisphere.
	// Disabled restores the exact legacy path.
	fragment_uniforms.bounce_color =
	        glm::vec4{lighting_.bounce_color, lighting_.bounce_intensity};
	fragment_uniforms.bounce_params =
	        glm::vec4{lighting_.bounce_enabled ? 1.0f : 0.0f, 0.0f, 0.0f,
	                  0.0f};
	// Blob shadows: up to 4 analytic spheres on the direct lobe only.
	for (uint32_t i = 0; i < 4; ++i) {
		fragment_uniforms.blob_spheres[i] =
		        i < blob_count_ ? blob_spheres_[i] : glm::vec4{0.0f};
	}
	fragment_uniforms.blob_params =
	        glm::vec4{static_cast<float>(blob_count_), pbr::kBlobSunRadius,
	                  0.0f, 0.0f};
	// SSAO: scales fill only, never the sun. Disabled (or no texture yet)
	// binds white so the factor reads 1. The texture is the PREVIOUS
	// frame's AO (run_ssao_pass produces it at the end of the scene pass),
	// so there is one frame of lag and no circular dependency.
	const bool ao_on = ssao_on();
	fragment_uniforms.ssao_params =
	        glm::vec4{ao_on ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
	fragment_uniforms.ssao_texel =
	        ao_on ? glm::vec4{1.0f / static_cast<float>(ssao_target_width_),
	                            1.0f / static_cast<float>(ssao_target_height_),
	                            0.0f, 0.0f}
	                : glm::vec4{0.0f};

	const uint64_t vertex_offset =
	        push_uniforms(&vertex_uniforms, sizeof(vertex_uniforms));
	const uint64_t fragment_offset =
	        push_uniforms(&fragment_uniforms, sizeof(fragment_uniforms));
	VkDescriptorSet vertex_set   = alloc_vertex_set(vertex_offset);
	VkDescriptorSet fragment_set = alloc_fragment_set(fragment_offset);
	// Binding 1 (shadow map) lives here, next to the pipeline, so every draw
	// samples a valid descriptor: the map when lit, else the white texel
	// (depth 1.0 reads as lit, failing open). Binding 2 (SSAO) samples the
	// previous frame's AO texture, or white (AO=1) when off/unavailable.
	VkDescriptorSet sampler_set =
	        alloc_lit_sampler_set(white_texture_.view);
	context_.api.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             lit_pipeline_);
	const uint32_t vertex_dynamic   = static_cast<uint32_t>(vertex_offset);
	const uint32_t fragment_dynamic = static_cast<uint32_t>(fragment_offset);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   lit_layout_, 1, 1, &vertex_set, 1,
	                                   &vertex_dynamic);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   lit_layout_, 2, 1, &sampler_set, 0,
	                                   nullptr);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   lit_layout_, 3, 1, &fragment_set, 1,
	                                   &fragment_dynamic);
}

void GpuRendererVulkan::begin_scene_pass()
{
		if (cmd_ == VK_NULL_HANDLE || slot_ == nullptr ||
		    offscreen_color_.handle == VK_NULL_HANDLE) {
			return;
		}
		// Depth-only pass from the light, once per frame. Replays the draw list
		// commit_geometry_upload() kept; a frame with no geometry skips it and
		// the shader flag below takes the legacy unshadowed path.
		render_shadow_map();
		// FXAA: render into the intermediate, resolve onto the real target in
		// end_scene_pass(). The clear-only pass behind the editor stays direct:
		// there is nothing to antialias in a clear. A target that cannot host
		// the intermediate renders directly, so capture_rgba/write_ppm never
		// break.
		vk::Image* color_image       = &offscreen_color_;
		vk::ImageState* color_state  = &offscreen_color_state_;
		pass_redirected_             = false;
		sky_drawn_                   = false;
		pass_color_view_             = offscreen_color_.view;
		if (record_occlusion_ && lighting_.fxaa_enabled &&
		    !fxaa_unavailable_ && offscreen_width_ != 0 &&
		    offscreen_height_ != 0 && ensure_fxaa_pipeline() &&
		    ensure_fxaa_source(offscreen_width_, offscreen_height_)) {
			color_image       = &fxaa_source_;
			color_state       = &fxaa_source_state_;
			pass_redirected_  = true;
			pass_color_view_  = fxaa_source_.view;
		}
		// The pass's viewport: that of its first colour target, which is the
		// offscreen texture or the FXAA source made from it.
		pass_width_  = offscreen_width_;
		pass_height_ = offscreen_height_;

		image_as_color_attachment(*barriers_, cmd_, *color_image, 0, 1,
		                          *color_state);
		image_as_color_attachment(*barriers_, cmd_, pyramid_, 0, 1,
		                          pyramid_states_[0]);
		image_as_depth_attachment(*barriers_, cmd_, depth_, depth_state_);
		barriers_->flush(cmd_);
		begin_rendering(cmd_);
		scene_pass_open_ = true;
	}

void GpuRendererVulkan::draw(const glm::mat4& view, const glm::mat4& view_projection,
                             const glm::vec3& camera_position,
                             const GpuDrawBatch* batches, uint32_t batch_count,
                             const AssetRegistry* assets)
{
	if ((!scene_pass_open_ && !viewport_pass_open_) ||
	    cmd_ == VK_NULL_HANDLE || slot_ == nullptr ||
	    batches == nullptr || batch_count == 0 ||
	    lit_pipeline_ == VK_NULL_HANDLE) {
		return;
	}

	bool state_bound            = false;
	VkImageView bound_albedo    = VK_NULL_HANDLE;
	VkDescriptorSet sampler_set = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < batch_count; ++i) {
		const GpuDrawBatch& batch = batches[i];
		const bool instanced      = batch.mesh != kNoGpuMesh;
		if (instanced ? (batch.instance_count == 0 || !mesh_live(batch.mesh))
		              : batch.vertex_count == 0) {
			continue;
		}
		if (!state_bound) {
			// Geometry takes the lower half of the depth range and the sky,
			// below, the top of it (see kGeometryDepthMax).
			set_viewport(cmd_, pass_width_, pass_height_,
			             kGeometryDepthMax);
			set_scissor(cmd_, pass_width_, pass_height_);
			bind_lit_state(view, view_projection, camera_position);
			state_bound = true;
		}
		VkImageView albedo = texture_for_albedo(batch.albedo_id, assets);
		if (albedo != bound_albedo || sampler_set == VK_NULL_HANDLE) {
			sampler_set = alloc_lit_sampler_set(albedo);
			context_.api.CmdBindDescriptorSets(
			        cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, lit_layout_,
			        2, 1, &sampler_set, 0, nullptr);
			bound_albedo = albedo;
		}
		record_geometry_draw(cmd_, GeometryDraw{batch.mesh, batch.first_vertex,
		                                        batch.vertex_count,
		                                        batch.first_instance,
		                                        batch.instance_count});
	}

	// Background step for this pass: the sky dome, once, over the pixels no
	// geometry covers. It goes after the geometry so that its fragment shader
	// never runs where a surface already stands. A pass with no draws keeps
	// the legacy clear colour.
	if (state_bound) {
		draw_sky(view_projection, camera_position);
	}
}

void GpuRendererVulkan::end_scene_pass()
{
		if (!scene_pass_open_) {
			return;
		}
		scene_pass_open_ = false;
		if (cmd_ != VK_NULL_HANDLE) {
			context_.api.CmdEndRendering(cmd_);
			// FXAA resolve onto the real target. The pyramid reduction reads
			// target 1, which is never redirected, so resolve order vs. the
			// reduction below does not matter.
			if (pass_redirected_) {
				fxaa_resolve(offscreen_color_, offscreen_color_state_,
				             fxaa_width_, fxaa_height_);
			}
			pass_redirected_ = false;
			if (lighting_.ssao_enabled && !ssao_unavailable_ &&
			    pyramid_.handle != VK_NULL_HANDLE) {
				if (offscreen_width_ != 0 && offscreen_height_ != 0) {
					run_ssao_pass(offscreen_width_, offscreen_height_);
				}
			}
			// The reduction reads the colour target this pass just wrote, so
			// it can only be recorded after the render pass has ended. A
			// clear-only pass behind the editor suppresses this so it does
			// not replace the pyramid with an empty image.
			if (record_occlusion_) {
				record_pyramid();
			}
		}
		record_occlusion_ = true;
}

bool GpuRendererVulkan::ensure_sky_pipeline()
{
	if (!device_valid_) {
		return false;
	}
	if (sky_unavailable_) {
		return false;
	}
	if (sky_pipeline_ != VK_NULL_HANDLE && sky_format_ == kColorFormat) {
		return true;
	}
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	try {
		VkShaderModule vert = vk::create_shader_module(
		        api, device, sky_vert_spv, sizeof(sky_vert_spv));
		VkShaderModule frag = vk::create_shader_module(
		        api, device, sky_frag_spv, sizeof(sky_frag_spv));

		vk::GraphicsPipelineInfo sky{};
		sky.layout              = sky_layout_;
		sky.vertex              = vert;
		sky.fragment            = frag;
		sky.color_target_count  = 2;
		sky.color_formats[0]    = kColorFormat;
		sky.color_formats[1]    = kPyramidFormat;
		// The sky leaves the linear-depth target alone. Every pass clears that
		// target to kDepthFar, which is exactly what sky.frag writes, so the
		// write changes nothing; masked off, it is memory traffic saved on every
		// sky pixel.
		sky.mask_target1   = true;
		sky.depth_format   = kDepthFormat;
		sky.depth_test     = true;
		sky.depth_write    = false;
		sky.depth_compare  = VK_COMPARE_OP_LESS_OR_EQUAL;
		sky.vertex_input   = false;
		sky.depth_clamp    = true;
		sky_pipeline_      = vk::create_graphics_pipeline(
                api, device, sky, "sky");
		api.DestroyShaderModule(device, vert, nullptr);
		api.DestroyShaderModule(device, frag, nullptr);
	} catch (const std::exception& e) {
		std::cerr << "sky: background pipeline unavailable, "
		             "continuing with the clear color: " << e.what() << '\n';
		sky_unavailable_ = true;
		return false;
	}
	sky_format_ = kColorFormat;
	return true;
}

void GpuRendererVulkan::draw_sky(const glm::mat4& view_projection,
                                 const glm::vec3& camera_position)
{
	// Once per pass, after the geometry of the first draw() that has any:
	// over a pixel only where nothing was drawn, so geometry is never shaded
	// twice. A pass with no draws stays a clear. Disabled restores the exact
	// legacy clear-color path.
	if (sky_drawn_ || (!scene_pass_open_ && !viewport_pass_open_) ||
	    !lighting_.sky_enabled) {
		return;
	}
	if (!ensure_sky_pipeline()) {
		return;
	}
	sky_drawn_ = true;

	SkyVertexUniforms vertex_uniforms{};
	vertex_uniforms.inv_view_projection = glm::inverse(view_projection);

	SkyFragmentUniforms fragment_uniforms{};
	const glm::vec3 direction = glm::length(lighting_.direction) > 0.0f
	                                    ? glm::normalize(lighting_.direction)
	                                    : glm::vec3{0.0f, 0.0f, 1.0f};
	fragment_uniforms.camera_position = glm::vec4{camera_position, 1.0f};
	fragment_uniforms.sky_color       = glm::vec4{lighting_.sky_color, 0.0f};
	fragment_uniforms.ground_color    = glm::vec4{lighting_.ground_color, 0.0f};
	fragment_uniforms.sun_direction   = glm::vec4{direction, 0.0f};
	fragment_uniforms.sun_color =
	        glm::vec4{lighting_.color, lighting_.intensity};
	fragment_uniforms.params = glm::vec4{lighting_.exposure,
	                                     lighting_.horizon_warmth, 0.0f, 0.0f};

	const uint64_t vertex_offset =
	        push_uniforms(&vertex_uniforms, sizeof(vertex_uniforms));
	VkDescriptorSet vertex_set = alloc_vertex_set(vertex_offset);
	// sky.frag's block is smaller than lit.frag's; it still goes through the
	// same fragment-UBO layout, with a range the pipeline creation accepts.
	const uint64_t fragment_offset =
	        push_uniforms(&fragment_uniforms, sizeof(fragment_uniforms));
	VkDescriptorSet fragment_set = alloc_fragment_set(fragment_offset);
	VkDescriptorSet sampler_set  = alloc_sampler_set(
            white_texture_.view, default_sampler_, white_texture_.view,
            default_sampler_, white_texture_.view, default_sampler_);

	// The geometry just drawn used the lower half of the depth range; the sky
	// sits at the far end of the full one, above all of it.
	set_viewport(cmd_, pass_width_, pass_height_, 1.0f);
	set_scissor(cmd_, pass_width_, pass_height_);

	context_.api.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             sky_pipeline_);
	const uint32_t vertex_dynamic   = static_cast<uint32_t>(vertex_offset);
	const uint32_t fragment_dynamic = static_cast<uint32_t>(fragment_offset);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   sky_layout_, 1, 1, &vertex_set, 1,
	                                   &vertex_dynamic);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   sky_layout_, 2, 1, &sampler_set, 0,
	                                   nullptr);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   sky_layout_, 3, 1, &fragment_set, 1,
	                                   &fragment_dynamic);
	// Bufferless fullscreen triangle from sky.vert.
	context_.api.CmdDraw(cmd_, 3, 1, 0, 0);
}

} // namespace lotus
