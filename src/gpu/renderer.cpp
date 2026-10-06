#include "lotus/gpu/renderer.hpp"

#include "lotus/gpu/lit.frag.spv.h"
#include "lotus/gpu/lit.vert.spv.h"
#include "lotus/gpu/depth.comp.spv.h"

#include <cstring>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <stdexcept>

namespace lotus {
namespace {

constexpr uint32_t kInitialVertexBytes = 128 * 1024;
constexpr uint32_t kVertexStride       = sizeof(GpuVertex);
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

/// Rows in the readback transfer buffer are padded so each pitch is a multiple
/// of this. D3D12 requires 256; matching it everywhere avoids SDL inserting a
/// repacking copy on every download.
constexpr uint32_t kTransferRowAlignment = 256;

/// Must match shaders/lit.vert (set 1, binding 0).
struct VertexUniforms {
	glm::mat4 view_projection;
	glm::mat4 view;
};

// lit.vert declares these as std140 offsets 0 and 64. A mismatch here is a
// silent data bug, not a compile error: the pyramid would be reduced from an
// indeterminate view matrix and every depth would be plausible but wrong.
static_assert(sizeof(VertexUniforms) == 128, "must match lit.vert's std140 block");

/// Must match shaders/lit.frag (set 3, binding 0).
struct FragmentUniforms {
	glm::vec4 light_direction;
	glm::vec4 light_color;    // rgb + intensity in .a
	glm::vec4 ambient_color;
	glm::vec4 camera_position;
};

SDL_GPUShader* create_shader(SDL_GPUDevice* device, bool vertex)
{
	SDL_GPUShaderCreateInfo info{};
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.stage  = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
	info.entrypoint = "main";
	// Uniform buffers live in set 1 for vertex shaders, set 3 for fragment.
	info.num_uniform_buffers = 1;
	if (vertex) {
		info.code      = lit_vert_spv;
		info.code_size = lit_vert_spv_len;
	} else {
		info.code      = lit_frag_spv;
		info.code_size = lit_frag_spv_len;
	}
	SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
	if (!shader) {
		throw std::runtime_error(SDL_GetError());
	}
	return shader;
}

} // namespace

GpuRenderer::~GpuRenderer()
{
	shutdown();
}

void GpuRenderer::init(SDL_Window* window, bool debug_device)
{
	if (device_) {
		return;
	}
	window_ = window;
	device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug_device, "vulkan");
	if (!device_) {
		throw std::runtime_error(SDL_GetError());
	}
	if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
		SDL_DestroyGPUDevice(device_);
		device_ = nullptr;
		throw std::runtime_error(SDL_GetError());
	}
	swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(device_, window);
	occlusion_enabled_ = create_occlusion_pipeline();
	if (!occlusion_enabled_) {
		std::cerr << "occlusion: compute pipeline unavailable, "
		             "continuing without depth culling\n";
	}
	// The swapchain size is not known until the first frame, so the pyramid is
	// built then; ensure_pyramid rebuilds this pipeline when it appears.
	create_pipeline();
	ensure_vertex_capacity(kInitialVertexBytes);
}

void GpuRenderer::init_offscreen(uint32_t width, uint32_t height)
{
	if (device_) {
		return;
	}
	if (width == 0 || height == 0) {
		throw std::runtime_error("offscreen size must be non-zero");
	}
	offscreen_ = true;
	device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
	if (!device_) {
		offscreen_ = false;
		throw std::runtime_error(SDL_GetError());
	}
	swapchain_format_  = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	offscreen_width_   = width;
	offscreen_height_  = height;

	SDL_GPUTextureCreateInfo color{};
	color.type                 = SDL_GPU_TEXTURETYPE_2D;
	color.format               = swapchain_format_;
	color.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
	                             SDL_GPU_TEXTUREUSAGE_SAMPLER;
	color.width                = width;
	color.height               = height;
	color.layer_count_or_depth = 1;
	color.num_levels           = 1;
	color.sample_count         = SDL_GPU_SAMPLECOUNT_1;
	offscreen_color_           = SDL_CreateGPUTexture(device_, &color);
	if (!offscreen_color_) {
		SDL_DestroyGPUDevice(device_);
		device_    = nullptr;
		offscreen_ = false;
		throw std::runtime_error(SDL_GetError());
	}

	occlusion_enabled_ = create_occlusion_pipeline();
	if (!occlusion_enabled_) {
		std::cerr << "occlusion: compute pipeline unavailable, "
		             "continuing without depth culling\n";
	}
	ensure_depth_target(width, height);
	ensure_pyramid(width, height);
	create_pipeline();
	ensure_vertex_capacity(kInitialVertexBytes);
}

void GpuRenderer::create_pipeline()
{
	SDL_GPUShader* vert = create_shader(device_, true);
	SDL_GPUShader* frag = create_shader(device_, false);

	SDL_GPUColorTargetDescription color_targets[2]{};
	color_targets[0].format = swapchain_format_;
	// Must be R32_FLOAT: depth.comp declares both images `r32f`, and a
	// descriptor whose view format differs is a Vulkan validation error.
	color_targets[1].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	// Always two targets. lit.frag unconditionally writes location 1, so a
	// one-target pipeline is a shader/attachment interface mismatch that
	// Vulkan rejects at pipeline-creation time. The pyramid texture is
	// therefore allocated unconditionally; only the compute reduction and the
	// readback are gated on occlusion_enabled_.

	if (pipeline_) {
		SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
		pipeline_ = nullptr;
	}

	SDL_GPUVertexBufferDescription vb_desc{};
	vb_desc.slot               = 0;
	vb_desc.pitch              = kVertexStride;
	vb_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vb_desc.instance_step_rate = 0;

	SDL_GPUVertexAttribute attrs[3]{};
	attrs[0].location    = 0;
	attrs[0].buffer_slot = 0;
	attrs[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	attrs[0].offset      = offsetof(GpuVertex, position);
	attrs[1].location    = 1;
	attrs[1].buffer_slot = 0;
	attrs[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	attrs[1].offset      = offsetof(GpuVertex, normal);
	attrs[2].location    = 2;
	attrs[2].buffer_slot = 0;
	attrs[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	attrs[2].offset      = offsetof(GpuVertex, color);

	SDL_GPUGraphicsPipelineCreateInfo pipe{};
	pipe.vertex_shader   = vert;
	pipe.fragment_shader = frag;
	pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipe.target_info.num_color_targets         = 2u;
	pipe.target_info.color_target_descriptions = color_targets;
	pipe.target_info.has_depth_stencil_target  = true;
	pipe.target_info.depth_stencil_format      = kDepthFormat;
	pipe.vertex_input_state.num_vertex_buffers = 1;
	pipe.vertex_input_state.vertex_buffer_descriptions = &vb_desc;
	pipe.vertex_input_state.num_vertex_attributes      = 3;
	pipe.vertex_input_state.vertex_attributes          = attrs;
	pipe.rasterizer_state.cull_mode   = SDL_GPU_CULLMODE_NONE;
	pipe.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	pipe.depth_stencil_state.enable_depth_test  = true;
	pipe.depth_stencil_state.enable_depth_write = true;
	pipe.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;

	pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipe);
	SDL_ReleaseGPUShader(device_, vert);
	SDL_ReleaseGPUShader(device_, frag);
	if (!pipeline_) {
		throw std::runtime_error(SDL_GetError());
	}
}

void GpuRenderer::destroy_buffers()
{
	if (vertex_buffer_) {
		SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
		vertex_buffer_ = nullptr;
	}
	if (transfer_buffer_) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
		transfer_buffer_ = nullptr;
	}
	capacity_ = 0;
}

void GpuRenderer::ensure_vertex_capacity(uint32_t bytes)
{
	if (bytes <= capacity_) {
		return;
	}
	uint32_t next = capacity_ ? capacity_ : kInitialVertexBytes;
	while (next < bytes) {
		next *= 2;
	}
	destroy_buffers();

	SDL_GPUBufferCreateInfo vb{};
	vb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	vb.size  = next;
	vertex_buffer_ = SDL_CreateGPUBuffer(device_, &vb);
	if (!vertex_buffer_) {
		throw std::runtime_error(SDL_GetError());
	}

	SDL_GPUTransferBufferCreateInfo tb{};
	tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tb.size  = next;
	transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &tb);
	if (!transfer_buffer_) {
		throw std::runtime_error(SDL_GetError());
	}
	capacity_ = next;
}

bool GpuRenderer::create_occlusion_pipeline()
{
	SDL_GPUComputePipelineCreateInfo info{};
	info.code       = depth_comp_spv;
	info.code_size  = depth_comp_spv_len;
	info.entrypoint = "main";
	info.format     = SDL_GPU_SHADERFORMAT_SPIRV;
	// Both slots are image2D storage textures. SDL_BindGPUComputeStorageTextures
	// takes bare textures with no mip level, and depth.comp needs a specific
	// (src, dst) mip pair per dispatch, so read-write bindings are the only
	// option.
	info.num_readwrite_storage_textures = 2;
	info.threadcount_x                  = 8;
	info.threadcount_y                  = 8;
	info.threadcount_z                  = 1;

	hiz_pipeline_ = SDL_CreateGPUComputePipeline(device_, &info);
	return hiz_pipeline_ != nullptr;
}

// Size-scoped teardown: everything that depends on the frame's dimensions.
// The compute pipeline is device-scoped and deliberately survives a resize.
void GpuRenderer::destroy_pyramid()
{
	if (pyramid_fence_) {
		// Retired, never released outright. This fence's vkQueueSubmit may
		// still be pending, and releasing it drops the reference count to
		// zero and returns the handle to SDL's pool — where the next submit
		// pops it and calls vkResetFences on a fence Vulkan still considers in
		// use. A prematurely reset fence also reports signalled, which would
	// let the poll map a download that has not landed. release_retired_fences
		// collects each one only once SDL_QueryGPUFence agrees.
		retired_fences_.push_back(pyramid_fence_);
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
	pyramid_recorded_  = false;
	occlusion_pyramid_.clear();
	occlusion_ready_   = false;
}

void GpuRenderer::destroy_occlusion()
{
	destroy_pyramid();
	if (hiz_pipeline_) {
		SDL_ReleaseGPUComputePipeline(device_, hiz_pipeline_);
		hiz_pipeline_ = nullptr;
	}
	occlusion_enabled_ = false;
}

void GpuRenderer::release_retired_fences()
{
	for (auto it = retired_fences_.begin(); it != retired_fences_.end();) {
		// Query, never wait. A fence that is not signalled yet stays retired
		// and is retried next frame; a resize must not stall the loop.
		if (SDL_QueryGPUFence(device_, *it)) {
			SDL_ReleaseGPUFence(device_, *it);
			it = retired_fences_.erase(it);
		} else {
			++it;
		}
	}
}

void GpuRenderer::ensure_pyramid(uint32_t width, uint32_t height)
{
	if (pyramid_reduction_unavailable_) {
		// The reduction is permanently off. A resize must still be able to
		// refresh the stand-in texture the scene pass writes as colour target
		// 1, but the mip chain is never attempted again.
		if (pyramid_texture_ && pyramid_width_ == width &&
		    pyramid_height_ == height) {
			return;
		}
		destroy_pyramid();
		if (width != 0 && height != 0) {
			ensure_pyramid_stand_in(width, height);
		}
		return;
	}
	if (pyramid_texture_ && pyramid_width_ == width &&
	    pyramid_height_ == height) {
		return;
	}

	// Anything referencing the old texture is dropped rather than waited on.
	destroy_pyramid();

	// The texture is created whether or not the compute pipeline exists: mip 0
	// is written by the scene pass as colour target 1, which the pipeline
	// declares unconditionally. Occlusion-disabled therefore means "no
	// reduction, no culling", not "a different pipeline".
	if (width != 0 && height != 0) {
		ensure_pyramid_texture(width, height);
	}
}

/// Colour target 1 when the reduction is unavailable: one R32F mip, no mip
/// chain and no compute usage. lit.frag writes location 1 unconditionally, so
/// the scene pass cannot simply drop the attachment — that would leave the
/// render pass with one target against a two-target pipeline. Writing the
/// depth into a texture nothing ever reads keeps the two in agreement and
/// costs one mip.
void GpuRenderer::ensure_pyramid_stand_in(uint32_t width, uint32_t height)
{
	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	info.width                = width;
	info.height               = height;
	info.layer_count_or_depth = 1;
	info.num_levels           = 1;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	pyramid_texture_ = SDL_CreateGPUTexture(device_, &info);
	if (!pyramid_texture_) {
		// Not an occlusion failure any more: lit.frag writes colour target 1
		// for every draw, so an R32F colour target is a rendering requirement
		// and the scene pass cannot be formed without one. Matches
		// ensure_depth_target, which throws on the same class of failure.
		throw std::runtime_error(std::string("cannot create the linear-depth "
		                                    "colour target: ") +
		                         SDL_GetError());
	}
	pyramid_width_  = width;
	pyramid_height_ = height;
	pyramid_levels_ = 0; // nothing to reduce and nothing to read back
}

void GpuRenderer::ensure_pyramid_texture(uint32_t width, uint32_t height)
{
	// Only mips 0..K exist: the query reads mip K and nothing coarser, so
	// reducing further and transferring more would be pure waste.
	const uint32_t levels = kDepthPyramidMinLevel + 1;

	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	// SIMULTANEOUS_READ_WRITE, never READ | WRITE: depth.comp reads and writes
	// storage images, and SDL_gpu.h warns the two are not equivalent.
	info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
	                             SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
	info.width                = width;
	info.height               = height;
	info.layer_count_or_depth = 1;
	info.num_levels           = levels;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	pyramid_texture_ = SDL_CreateGPUTexture(device_, &info);
	if (!pyramid_texture_) {
		// Degrade rather than leave pyramid_texture_ null: the scene pass binds
		// it as colour target 1 unconditionally, and SDL's Vulkan backend
		// dereferences colorTargetInfos[i].texture with no null check, so a
		// null here is a crash inside the driver, not a skipped feature.
		//
		// The flag makes this permanent. ensure_pyramid matches on
		// pyramid_texture_ being non-null, so without it the next frame would
		// retry and log again, forever, for a cause that is a missing usage
		// flag or format rather than the size.
		pyramid_reduction_unavailable_ = true;
		occlusion_enabled_             = false;
		std::cerr << "occlusion: depth pyramid unavailable, continuing without "
		             "occlusion culling: " << SDL_GetError() << '\n';
		ensure_pyramid_stand_in(width, height);
		return;
	}

	// Only mips from kDepthPyramidMinLevel are ever transferred; the rest get
	// offset 0 and are never read.
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
			occlusion_enabled_ = false;
			std::cerr << "occlusion: transfer buffer unavailable: "
			          << SDL_GetError() << '\n';
		}
	}

	pyramid_width_  = width;
	pyramid_height_ = height;
	pyramid_levels_ = levels;
}

void GpuRenderer::poll_pyramid_fence()
{
	// Drains fences a resize superseded. Polls, never waits.
	release_retired_fences();
	occlusion_ready_ = false;
	if (!pyramid_in_flight_ || !pyramid_fence_) {
		return;
	}

	// This SDL build's SDL_QueryGPUFence returns true when the fence is
	// signaled and false when it is not (or on error); both mean the same
	// thing here: nothing to read back yet.
	if (!SDL_QueryGPUFence(device_, pyramid_fence_)) {
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
	     ++level) { // exactly one iteration: only mip K is read
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

	// adopt() rejects a chain whose transferred mips disagree with the
	// accessors and leaves the pyramid empty, so readiness is read back off
	// the object rather than assumed. Reporting ready after a rejection is the
	// false reassurance an empty pyramid is worst at: an empty pyramid never
	// occludes, so the failure is invisible in the culling counts.
	occlusion_pyramid_.adopt(pyramid_width_, pyramid_height_, pyramid_levels_,
	                         mips);
	occlusion_ready_ = !occlusion_pyramid_.empty();
}

void GpuRenderer::record_pyramid()
{
	// Gated on occlusion_enabled_, not merely on hiz_pipeline_: the pyramid
	// texture always exists because the scene pass writes it as color target 1,
	// so without this the reduction would still run pointlessly whenever
	// occlusion was disabled.
	//
	// pyramid_in_flight_ keeps this to at most one outstanding fence. Recording
	// a second download into the same transfer buffer while the first
	// submission is still pending would overwrite the bytes the poll is going
	// to map, and would leave end_frame holding a fence nobody has consumed —
	// releasing that is the premature-vkResetFences bug. Skipping the download
	// costs one frame of occlusion, which is the direction this project calls
	// safe: the frame still renders, it just culls nothing.
	if (!occlusion_enabled_ || !pyramid_texture_ || !hiz_pipeline_ ||
	    !pyramid_transfer_ || !cmd_ || pyramid_in_flight_ ||
	    pyramid_levels_ <= kDepthPyramidMinLevel) {
		return;
	}

	// One compute pass per level: SDL fixes storage bindings when a pass
	// begins, so the mip pair cannot change within one pass.
	for (uint32_t level = 1; level <= kDepthPyramidMinLevel; ++level) {
		SDL_GPUStorageTextureReadWriteBinding bindings[2]{};
		bindings[0].texture   = pyramid_texture_;
		bindings[0].mip_level = level - 1;
		bindings[0].layer     = 0;
		bindings[1].texture   = pyramid_texture_;
		bindings[1].mip_level = level;
		bindings[1].layer     = 0;

		SDL_GPUComputePass* compute =
	        SDL_BeginGPUComputePass(cmd_, bindings, 2, nullptr, 0);
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
	for (uint32_t level = kDepthPyramidMinLevel; level < pyramid_levels_;
	     ++level) { // mip K only: coarser levels are never built or read
		const uint32_t w = std::max(1u, pyramid_width_ >> level);
		const uint32_t h = std::max(1u, pyramid_height_ >> level);
		const uint32_t pitch =
		        ((w * sizeof(float) + kTransferRowAlignment - 1) /
		         kTransferRowAlignment) *
		        kTransferRowAlignment;

		SDL_GPUTextureRegion source{};
		source.texture  = pyramid_texture_;
		source.mip_level = level;
		source.layer    = 0;
		source.w        = w;
		source.h        = h;
		source.d        = 1;

		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = pyramid_transfer_;
		destination.offset          = pyramid_offset_[level];
		// pixels_per_row IS the destination row stride, so it must be the
		// padded pitch expressed in pixels. Setting it to the raw width makes
		// the GPU write row y at y * w * 4 while the unpack walks y * pitch,
		// so every row after the first is garbage whenever
		// w * sizeof(float) is not a multiple of 256.
		destination.pixels_per_row = pitch / sizeof(float);
		destination.rows_per_layer = h;

		SDL_DownloadFromGPUTexture(copy, &source, &destination);
	}
	SDL_EndGPUCopyPass(copy);
	pyramid_recorded_ = true;
}

void GpuRenderer::ensure_depth_target(uint32_t width, uint32_t height)
{
	if (depth_texture_ && depth_width_ == width && depth_height_ == height) {
		return;
	}
	if (depth_texture_) {
		SDL_ReleaseGPUTexture(device_, depth_texture_);
		depth_texture_ = nullptr;
	}
	if (width == 0 || height == 0) {
		depth_width_  = 0;
		depth_height_ = 0;
		return;
	}

	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = kDepthFormat;
	info.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	info.width                = width;
	info.height               = height;
	info.layer_count_or_depth = 1;
	info.num_levels           = 1;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
	depth_texture_            = SDL_CreateGPUTexture(device_, &info);
	if (!depth_texture_) {
		throw std::runtime_error(SDL_GetError());
	}
	depth_width_  = width;
	depth_height_ = height;
}

void GpuRenderer::begin_frame_offscreen()
{
	if (!offscreen_ || !offscreen_color_) {
		return;
	}
	poll_pyramid_fence();
	cmd_ = SDL_AcquireGPUCommandBuffer(device_);
	if (!cmd_) {
		throw std::runtime_error(SDL_GetError());
	}
	swapchain_ = offscreen_color_;
	ensure_depth_target(offscreen_width_, offscreen_height_);
}

std::vector<uint8_t> GpuRenderer::capture_rgba(uint32_t& width, uint32_t& height)
{
	width  = 0;
	height = 0;
	if (!offscreen_ || !offscreen_color_) {
		return {};
	}
	SDL_WaitForGPUIdle(device_);

	width  = offscreen_width_;
	height = offscreen_height_;
	const uint32_t byte_size = width * height * 4;

	SDL_GPUTransferBufferCreateInfo transfer_info{};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	transfer_info.size  = byte_size;
	SDL_GPUTransferBuffer* transfer =
	        SDL_CreateGPUTransferBuffer(device_, &transfer_info);
	if (!transfer) {
		throw std::runtime_error(SDL_GetError());
	}

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
	SDL_GPUCopyPass* pass     = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion region{};
	region.texture = offscreen_color_;
	region.w       = width;
	region.h       = height;
	region.d       = 1;
	SDL_GPUTextureTransferInfo destination{};
	destination.transfer_buffer = transfer;
	destination.offset          = 0;
	SDL_DownloadFromGPUTexture(pass, &region, &destination);
	SDL_EndGPUCopyPass(pass);
	if (!SDL_SubmitGPUCommandBuffer(cmd)) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		throw std::runtime_error(SDL_GetError());
	}
	SDL_WaitForGPUIdle(device_);

	std::vector<uint8_t> pixels(byte_size);
	const void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
	if (!mapped) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		throw std::runtime_error(SDL_GetError());
	}
	std::memcpy(pixels.data(), mapped, byte_size);
	SDL_UnmapGPUTransferBuffer(device_, transfer);
	SDL_ReleaseGPUTransferBuffer(device_, transfer);
	return pixels;
}

bool GpuRenderer::resize_offscreen(uint32_t width, uint32_t height)
{
	if (!offscreen_ || !offscreen_color_ || width == 0 || height == 0) {
		return false;
	}
	if (width == offscreen_width_ && height == offscreen_height_) {
		return true;
	}

	SDL_GPUTextureCreateInfo color{};
	color.type                 = SDL_GPU_TEXTURETYPE_2D;
	color.format               = swapchain_format_;
	color.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
	                             SDL_GPU_TEXTUREUSAGE_SAMPLER;
	color.width                = width;
	color.height               = height;
	color.layer_count_or_depth = 1;
	color.num_levels           = 1;
	color.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	// Created before the old one is released, so a failed resize leaves the
	// renderer on its old target rather than on none.
	SDL_GPUTexture* replacement = SDL_CreateGPUTexture(device_, &color);
	if (!replacement) {
		return false;
	}
	SDL_ReleaseGPUTexture(device_, offscreen_color_);
	offscreen_color_  = replacement;
	offscreen_width_  = width;
	offscreen_height_ = height;

	// Both are sized from the target, and both drop what they had: the depth
	// buffer because it is now the wrong size, the pyramid because its mip
	// chain is. destroy_pyramid clears the CPU-side chain and occlusion_ready_,
	// so the frame after a resize treats every entity as visible rather than
	// querying a stale chain against the new camera.
	ensure_depth_target(width, height);
	ensure_pyramid(width, height);
	return true;
}

bool GpuRenderer::write_ppm(const std::string& path, std::string* error)
{
	uint32_t width  = 0;
	uint32_t height = 0;
	const std::vector<uint8_t> pixels = capture_rgba(width, height);
	if (pixels.empty()) {
		if (error) {
			*error = "no offscreen target to capture";
		}
		return false;
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) {
			*error = "cannot write " + path;
		}
		return false;
	}
	out << "P6\n" << width << ' ' << height << "\n255\n";
	for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
		out.put(static_cast<char>(pixels[i]));
		out.put(static_cast<char>(pixels[i + 1]));
		out.put(static_cast<char>(pixels[i + 2]));
	}
	return static_cast<bool>(out);
}

void GpuRenderer::shutdown()
{
	if (!device_) {
		return;
	}
	if (pass_) {
		SDL_EndGPURenderPass(pass_);
		pass_ = nullptr;
	}
	if (cmd_) {
		SDL_CancelGPUCommandBuffer(cmd_);
		cmd_ = nullptr;
	}
	SDL_WaitForGPUIdle(device_);
	destroy_occlusion();
	// Every retired fence is now signalled, so this only releases handles; it
	// cannot block. Releasing them here rather than leaking keeps SDL's fence
	// pool and the Vulkan device in step through SDL_DestroyGPUDevice.
	for (SDL_GPUFence* fence : retired_fences_) {
		SDL_ReleaseGPUFence(device_, fence);
	}
	retired_fences_.clear();

	destroy_buffers();
	if (depth_texture_) {
		SDL_ReleaseGPUTexture(device_, depth_texture_);
		depth_texture_ = nullptr;
	}
	if (offscreen_color_) {
		SDL_ReleaseGPUTexture(device_, offscreen_color_);
		offscreen_color_ = nullptr;
	}
	if (pipeline_) {
		SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
		pipeline_ = nullptr;
	}
	if (window_) {
		SDL_ReleaseWindowFromGPUDevice(device_, window_);
		window_ = nullptr;
	}
	SDL_DestroyGPUDevice(device_);
	device_ = nullptr;
}

void GpuRenderer::begin_frame(SDL_Window* window, float clear_r, float clear_g,
                              float clear_b)
{
	clear_r_ = clear_r;
	clear_g_ = clear_g;
	clear_b_ = clear_b;

	// Before anything else, so occlusion for this frame uses the previous
	// frame's pyramid. Polls, never waits.
	poll_pyramid_fence();

	cmd_ = SDL_AcquireGPUCommandBuffer(device_);
	if (!cmd_) {
		throw std::runtime_error(SDL_GetError());
	}
	// Non-blocking: the blocking variant can stall indefinitely on some
	// drivers/compositors (observed: NVIDIA/GLX drmSyncobjTimelineWait).
	// Callers should skip the frame when has_frame() is false.
	swapchain_ = nullptr;
	Uint32 width = 0;
	Uint32 height = 0;
	if (!SDL_AcquireGPUSwapchainTexture(cmd_, window, &swapchain_, &width,
	                                    &height)) {
		throw std::runtime_error(SDL_GetError());
	}
	if (!swapchain_) {
		SDL_CancelGPUCommandBuffer(cmd_);
		cmd_ = nullptr;
		return;
	}
	ensure_depth_target(width, height);
	ensure_pyramid(width, height);
}

void GpuRenderer::upload_vertices(const GpuVertex* vertices, uint32_t count)
{
	if (!cmd_ || count == 0 || !vertices) {
		return;
	}
	const uint32_t bytes = count * static_cast<uint32_t>(sizeof(GpuVertex));
	ensure_vertex_capacity(bytes);

	void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_buffer_, true);
	if (!mapped) {
		throw std::runtime_error(SDL_GetError());
	}
	std::memcpy(mapped, vertices, bytes);
	SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);

	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = transfer_buffer_;
	src.offset          = 0;
	SDL_GPUBufferRegion dst{};
	dst.buffer = vertex_buffer_;
	dst.offset = 0;
	dst.size   = bytes;
	SDL_UploadToGPUBuffer(copy, &src, &dst, true);
	SDL_EndGPUCopyPass(copy);
}

void GpuRenderer::begin_scene_pass()
{
	if (!cmd_ || !swapchain_) {
		return;
	}
	SDL_GPUColorTargetInfo targets[2]{};
	targets[0].texture     = swapchain_;
	targets[0].clear_color = SDL_FColor{clear_r_, clear_g_, clear_b_, 1.0f};
	targets[0].load_op     = SDL_GPU_LOADOP_CLEAR;
	targets[0].store_op    = SDL_GPU_STOREOP_STORE;
	// Unconditional, matching the pipeline's two declared colour targets.
	// ensure_pyramid guarantees a texture here in every path — the mip chain,
	// or the single-mip stand-in when the reduction is unavailable — so the
	// render pass and the pipeline always agree. Asserted rather than assumed:
	// SDL's Vulkan backend dereferences colorTargetInfos[i].texture with no
	// null check, so a null would be a crash inside the driver rather than a
	// skipped frame.
	if (!pyramid_texture_) {
		throw std::runtime_error("scene pass has no linear-depth colour target");
	}
	// Load-bearing: an unwritten texel reads as ~0 and the min reduction would
	// spread that zero across the whole pyramid, culling everything.
	targets[1].texture     = pyramid_texture_;
	targets[1].mip_level   = 0;
	targets[1].clear_color = SDL_FColor{kDepthFar, kDepthFar, kDepthFar, kDepthFar};
	targets[1].load_op     = SDL_GPU_LOADOP_CLEAR;
	targets[1].store_op    = SDL_GPU_STOREOP_STORE;
	const Uint32 target_count = 2u;

	SDL_GPUDepthStencilTargetInfo depth{};
	depth.texture          = depth_texture_;
	depth.clear_depth      = 1.0f;
	depth.load_op          = SDL_GPU_LOADOP_CLEAR;
	depth.store_op         = SDL_GPU_STOREOP_DONT_CARE;
	depth.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

	pass_ = SDL_BeginGPURenderPass(cmd_, targets, target_count,
	                               depth_texture_ ? &depth : nullptr);
	if (!pass_) {
		throw std::runtime_error(SDL_GetError());
	}
}

void GpuRenderer::draw(const glm::mat4& view, const glm::mat4& view_projection,
                       const glm::vec3& camera_position, uint32_t count)
{
	if (!pass_ || !pipeline_ || count == 0) {
		return;
	}

	VertexUniforms vertex_uniforms{};
	vertex_uniforms.view_projection = view_projection;
	vertex_uniforms.view           = view;

	FragmentUniforms fragment_uniforms{};
	const glm::vec3 direction = glm::length(lighting_.direction) > 0.0f
	                                    ? glm::normalize(lighting_.direction)
	                                    : glm::vec3{0.0f, 0.0f, 1.0f};
	fragment_uniforms.light_direction = glm::vec4{direction, 0.0f};
	fragment_uniforms.light_color =
	        glm::vec4{lighting_.color, lighting_.intensity};
	fragment_uniforms.ambient_color   = glm::vec4{lighting_.ambient, 0.0f};
	fragment_uniforms.camera_position = glm::vec4{camera_position, 1.0f};

	SDL_PushGPUVertexUniformData(cmd_, 0, &vertex_uniforms,
	                             sizeof(vertex_uniforms));
	SDL_PushGPUFragmentUniformData(cmd_, 0, &fragment_uniforms,
	                               sizeof(fragment_uniforms));

	SDL_BindGPUGraphicsPipeline(pass_, pipeline_);
	SDL_GPUBufferBinding binding{};
	binding.buffer = vertex_buffer_;
	binding.offset = 0;
	SDL_BindGPUVertexBuffers(pass_, 0, &binding, 1);
	SDL_DrawGPUPrimitives(pass_, count, 1, 0, 0);
}

void GpuRenderer::end_scene_pass()
{
	if (pass_) {
		SDL_EndGPURenderPass(pass_);
		pass_ = nullptr;
		// The reduction reads the colour target this pass just wrote, so it can
		// only be recorded after the render pass has ended.
		record_pyramid();
	}
}

void GpuRenderer::end_frame()
{
	end_scene_pass();
	if (cmd_) {
		// Captured before cmd_ is cleared: the submit still needs the buffer,
		// and cmd_ must not dangle if a submit throws.
		SDL_GPUCommandBuffer* const submit = cmd_;
		const bool had_pyramid_download    = pyramid_recorded_;

		// No fence is dropped here. pyramid_recorded_ is only ever true when
		// pyramid_in_flight_ was false, so this submit cannot orphan one: the
		// fence it acquires is the single outstanding one, and
		// poll_pyramid_fence releases it once SDL_QueryGPUFence says it has
		// signalled.
		pyramid_recorded_ = false;
		cmd_              = nullptr;
		++frames_rendered_;

		if (had_pyramid_download) {
			pyramid_fence_ = SDL_SubmitGPUCommandBufferAndAcquireFence(submit);
			if (!pyramid_fence_) {
				// SDL's Vulkan backend has already ended this command buffer
				// inside the failed call, so there is nothing left to fall back
				// to: re-submitting would submit an already-ended buffer. Report
				// it the way every other submit failure in this file is
				// reported.
				throw std::runtime_error(SDL_GetError());
			}
			pyramid_in_flight_ = true;
		} else if (!SDL_SubmitGPUCommandBuffer(submit)) {
			throw std::runtime_error(SDL_GetError());
		}
	}
	swapchain_ = nullptr;
}

const char* GpuRenderer::driver_name() const
{
	return device_ ? SDL_GetGPUDeviceDriver(device_) : "";
}

} // namespace lotus
