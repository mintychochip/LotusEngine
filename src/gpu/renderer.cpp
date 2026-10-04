#include "lotus/gpu/renderer.hpp"

#include "lotus/gpu/cube.frag.spv.h"
#include "lotus/gpu/cube.vert.spv.h"

#include <cstring>
#include <stdexcept>

namespace lotus {
namespace {

SDL_GPUShader* create_shader(SDL_GPUDevice* device, bool vertex)
{
	SDL_GPUShaderCreateInfo info{};
	info.num_uniform_buffers = vertex ? 1u : 0u;
	info.format              = SDL_GPU_SHADERFORMAT_SPIRV;
	info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
	info.entrypoint          = "main";
	if (vertex) {
		info.code      = cube_vert_spv;
		info.code_size = cube_vert_spv_len;
	} else {
		info.code      = cube_frag_spv;
		info.code_size = cube_frag_spv_len;
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

	if (pipeline_) {
		SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
		pipeline_ = nullptr;
	}
	if (vertex_buffer_) {
		SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
		vertex_buffer_ = nullptr;
	}
	if (claimed_window_) {
		SDL_ReleaseWindowFromGPUDevice(device_, claimed_window_);
		claimed_window_ = nullptr;
	}
	SDL_DestroyGPUDevice(device_);
	device_ = nullptr;
}

void GpuRenderer::init(SDL_Window* window, bool debug_device)
{
	if (device_) {
		return;
	}
	claimed_window_ = window;
	device_ =
	        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug_device, "vulkan");
	if (!device_) {
		claimed_window_ = nullptr;
		throw std::runtime_error(SDL_GetError());
	}
	if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
		SDL_DestroyGPUDevice(device_);
		device_          = nullptr;
		claimed_window_  = nullptr;
		throw std::runtime_error(SDL_GetError());
	}

	SDL_GPUShader* vert = create_shader(device_, true);
	SDL_GPUShader* frag = create_shader(device_, false);

	SDL_GPUColorTargetDescription color_target{};
	color_target.format = SDL_GetGPUSwapchainTextureFormat(device_, window);

	SDL_GPUVertexBufferDescription vb_desc{};
	vb_desc.slot               = 0;
	vb_desc.pitch              = sizeof(float) * 6;
	vb_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vb_desc.instance_step_rate = 0;

	SDL_GPUVertexAttribute attrs[2]{};
	attrs[0].location    = 0;
	attrs[0].buffer_slot = 0;
	attrs[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	attrs[0].offset      = 0;
	attrs[1].location    = 1;
	attrs[1].buffer_slot = 0;
	attrs[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	attrs[1].offset      = sizeof(float) * 3;

	SDL_GPUGraphicsPipelineCreateInfo pipe{};
	pipe.vertex_shader   = vert;
	pipe.fragment_shader = frag;
	pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipe.target_info.num_color_targets         = 1;
	pipe.target_info.color_target_descriptions   = &color_target;
	pipe.target_info.has_depth_stencil_target    = false;
	pipe.vertex_input_state.num_vertex_buffers   = 1;
	pipe.vertex_input_state.vertex_buffer_descriptions = &vb_desc;
	pipe.vertex_input_state.num_vertex_attributes      = 2;
	pipe.vertex_input_state.vertex_attributes          = attrs;
	pipe.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

	pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipe);
	SDL_ReleaseGPUShader(device_, vert);
	SDL_ReleaseGPUShader(device_, frag);
	if (!pipeline_) {
		throw std::runtime_error(SDL_GetError());
	}

	SDL_GPUBufferCreateInfo buf_info{};
	buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buf_info.size  = vb_desc.pitch * 6;
	vertex_buffer_ = SDL_CreateGPUBuffer(device_, &buf_info);
	if (!vertex_buffer_) {
		throw std::runtime_error(SDL_GetError());
	}
}

void GpuRenderer::set_vertices(const void* data, uint32_t byte_size)
{
	if (!device_ || !vertex_buffer_) {
		return;
	}

	SDL_GPUTransferBufferCreateInfo tb_info{};
	tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tb_info.size  = byte_size;
	SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tb_info);
	if (!tb) {
		throw std::runtime_error(SDL_GetError());
	}

	void* map = SDL_MapGPUTransferBuffer(device_, tb, false);
	std::memcpy(map, data, byte_size);
	SDL_UnmapGPUTransferBuffer(device_, tb);

	SDL_GPUCommandBuffer* copy_cmd = SDL_AcquireGPUCommandBuffer(device_);
	SDL_GPUCopyPass* copy_pass     = SDL_BeginGPUCopyPass(copy_cmd);
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = tb;
	src.offset            = 0;
	SDL_GPUBufferRegion dst{};
	dst.buffer = vertex_buffer_;
	dst.offset = 0;
	dst.size   = byte_size;
	SDL_UploadToGPUBuffer(copy_pass, &src, &dst, false);
	SDL_EndGPUCopyPass(copy_pass);
	SDL_SubmitGPUCommandBuffer(copy_cmd);
	SDL_ReleaseGPUTransferBuffer(device_, tb);
}

void GpuRenderer::begin_frame(SDL_Window* window, float clear_r, float clear_g,
                              float clear_b)
{
	cmd_ = SDL_AcquireGPUCommandBuffer(device_);
	if (!cmd_) {
		throw std::runtime_error(SDL_GetError());
	}

	SDL_GPUTexture* swap = nullptr;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_, window, &swap, nullptr,
	                                           nullptr)) {
		throw std::runtime_error(SDL_GetError());
	}
	if (!swap) {
		SDL_CancelGPUCommandBuffer(cmd_);
		cmd_ = nullptr;
		return;
	}

	SDL_GPUColorTargetInfo color{};
	color.texture     = swap;
	color.clear_color = SDL_FColor{clear_r, clear_g, clear_b, 1.0f};
	color.load_op     = SDL_GPU_LOADOP_CLEAR;
	color.store_op    = SDL_GPU_STOREOP_STORE;

	pass_ = SDL_BeginGPURenderPass(cmd_, &color, 1, nullptr);
	if (!pass_) {
		throw std::runtime_error(SDL_GetError());
	}
}

void GpuRenderer::draw(const float* mvp, uint32_t vertex_count)
{
	if (!pass_ || !pipeline_) {
		return;
	}
	SDL_PushGPUVertexUniformData(cmd_, 0, mvp, sizeof(float) * 16);
	SDL_BindGPUGraphicsPipeline(pass_, pipeline_);
	SDL_GPUBufferBinding binding{};
	binding.buffer = vertex_buffer_;
	binding.offset = 0;
	SDL_BindGPUVertexBuffers(pass_, 0, &binding, 1);
	SDL_DrawGPUPrimitives(pass_, vertex_count, 1, 0, 0);
}

void GpuRenderer::end_frame()
{
	if (pass_) {
		SDL_EndGPURenderPass(pass_);
		pass_ = nullptr;
	}
	if (cmd_) {
		if (!SDL_SubmitGPUCommandBuffer(cmd_)) {
			throw std::runtime_error(SDL_GetError());
		}
		cmd_ = nullptr;
	}
}

const char* GpuRenderer::driver_name() const
{
	if (!device_) {
		return "";
	}
	return SDL_GetGPUDeviceDriver(device_);
}

} // namespace lotus
