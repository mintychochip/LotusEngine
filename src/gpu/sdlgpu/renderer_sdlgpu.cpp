#include "gpu/sdlgpu/renderer_sdlgpu.hpp"

#include "lotus/scene/assets.hpp"
#include "stb_image.h"

#include "lotus/gpu/lit.frag.spv.h"
#include "lotus/gpu/lit.vert.spv.h"
#include "lotus/gpu/depth.comp.spv.h"
#include "lotus/gpu/shadow.vert.spv.h"
#include "lotus/gpu/shadow.frag.spv.h"
#include "lotus/gpu/fxaa.vert.spv.h"
#include "lotus/gpu/fxaa.frag.spv.h"
#include "lotus/gpu/sky.vert.spv.h"
#include "lotus/gpu/sky.frag.spv.h"
#include "lotus/gpu/ssao.vert.spv.h"
#include "lotus/gpu/ssao.frag.spv.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cmath>
#include <stdexcept>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>

namespace lotus {
namespace {

constexpr uint32_t kInitialVertexBytes = 128 * 1024;
constexpr uint32_t kVertexStride       = sizeof(GpuVertex);
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

/// Geometry in the lit pass is rasterised into the lower half of the depth
/// range, [0, kGeometryDepthMax], through the viewport; the sky is drawn last,
/// at depth 1.0, into the half no geometry reaches.
///
/// The pipelines clamp depth rather than clip it, so geometry beyond the far
/// plane lands at the top of its range. With the full range that is 1.0, the
/// value a pixel nothing has touched keeps, and a sky drawn after the geometry
/// could not tell the two apart: it would paint over every ground plane that
/// runs past the far plane. With this range clamped geometry lands at 0.5 and
/// "depth 1.0" means "untouched".
///
/// A power of two, so that this is exact. Depth is a float, and scaling a
/// float by a power of two only moves its exponent: every depth the full range
/// produces maps to a distinct value, in the same order, with the same ties.
/// The picture is the one the full range gives, pixel for pixel.
constexpr float kGeometryDepthMax = 0.5f;

/// Shadow map: 1024px D16 depth rendered from the light each frame, then
/// sampled by lit.frag with a 3x3 PCF. D16 (not D32) because it is the one
/// depth format universally sampler-supported; devices that reject even this
/// combination take the legacy unshadowed path via shadows_supported_.
constexpr uint32_t kShadowMapSize = 1024;
constexpr SDL_GPUTextureFormat kShadowFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
/// Constant bias in shadow-depth units plus a world-space normal offset.
/// Matches shadow_params.y/z uploaded to lit.frag.
constexpr float kShadowBias         = 0.0015f;
constexpr float kShadowNormalOffset = 0.05f;
/// Fixed ortho box around the origin the light frustum covers. Keeps every
/// renderer entry point signature-stable (no scene bounds needed); large
/// scenes outside the box read as lit, failing open rather than into
/// darkness.
constexpr float kShadowOrthoHalf     = 15.0f;
constexpr float kShadowLightDistance = 30.0f;
constexpr float kShadowNear          = 1.0f;
constexpr float kShadowFar           = 60.0f;

/// Rows in the readback transfer buffer are padded so each pitch is a multiple
/// of this. D3D12 requires 256; matching it everywhere avoids SDL inserting a
/// repacking copy on every download.
constexpr uint32_t kTransferRowAlignment = 256;

/// Copies \p count floats out of mapped GPU memory into ordinary memory.
///
/// Memory the GPU wrote and the CPU maps can be write-combined, i.e.
/// uncached: an ordinary load then pays a full memory round trip, and on an
/// integrated GPU copying the 230 KB depth pyramid that way took 0.85 ms a
/// frame, 50 times what the same copy costs from cached memory. A streaming
/// load (MOVNTDQA) fetches a whole 64-byte line through a dedicated buffer
/// instead, which is what that memory is fast at; on cached memory it is an
/// ordinary load. Without SSE4.1, or for an unaligned source, this is a
/// plain copy.
void read_mapped_floats(float* destination, const float* source,
                        std::size_t count)
{
	std::size_t done = 0;
#if defined(__SSE4_1__)
	if ((reinterpret_cast<std::uintptr_t>(source) & 15u) == 0) {
		// Four loads of one line back to back, so the line is fetched once.
		for (; done + 16 <= count; done += 16) {
			auto* line = reinterpret_cast<__m128i*>(
			        const_cast<float*>(source + done));
			const __m128i a = _mm_stream_load_si128(line);
			const __m128i b = _mm_stream_load_si128(line + 1);
			const __m128i c = _mm_stream_load_si128(line + 2);
			const __m128i d = _mm_stream_load_si128(line + 3);
			auto* out       = reinterpret_cast<__m128i*>(destination + done);
			_mm_storeu_si128(out, a);
			_mm_storeu_si128(out + 1, b);
			_mm_storeu_si128(out + 2, c);
			_mm_storeu_si128(out + 3, d);
		}
	}
#endif
	if (done < count) {
		std::memcpy(destination + done, source + done,
		            (count - done) * sizeof(float));
	}
}

/// Must match shaders/lit.vert (set 1, binding 0). shadow.vert reuses the
/// same layout: the renderer uploads the light VP in view_projection.
struct VertexUniforms {
	glm::mat4 view_projection;
	glm::mat4 view;
};

// lit.vert declares these as std140 offsets 0 and 64. A mismatch here is a
// silent data bug, not a compile error: the pyramid would be reduced from an
// indeterminate view matrix and every depth would be plausible but wrong.
static_assert(sizeof(VertexUniforms) == 128, "must match lit.vert's std140 block");

/// Must match shaders/lit.frag (set 3, binding 0). Only GAINS trailing
/// members over the original five vec4s, so old uniform data stays
/// prefix-compatible.
struct FragmentUniforms {
	glm::vec4 light_direction;
	glm::vec4 light_color;    // rgb + intensity in .a
	glm::vec4 ambient_color;
	glm::vec4 camera_position;
	glm::vec4 tone_params;    // x: exposure; yzw reserved
	glm::vec4 hemi_sky;       // rgb: sky (up) hemisphere colour
	glm::vec4 hemi_ground;    // rgb: ground (down) hemisphere colour
	glm::vec4 shadow_params;  // x: enabled; y: bias; z: normal offset; w: texel
	glm::mat4 light_vp;       // light view-projection for in-frag shadow UVs
	glm::vec4 fog_params;     // x: enabled; y: density; zw reserved
	glm::vec4 fog_color;      // rgb: linear fog color from sky/sun hues
	glm::vec4 bounce_color;   // rgb + intensity in .a
	glm::vec4 bounce_params;  // x: enabled; yzw reserved
	glm::vec4 blob_spheres[4]; // xyz: center; w: radius
	glm::vec4 blob_params;    // x: count; y: sun angular radius; zw reserved
	glm::vec4 ssao_params;    // x: enabled; yzw reserved
	glm::vec4 ssao_texel;     // xy: 1/width, 1/height; zw reserved
};

static_assert(sizeof(FragmentUniforms) == 368,
              "must match lit.frag's std140 block");

/// Must match shaders/ssao.frag (set 3, binding 0).
struct SsaoUniforms {
	glm::vec4 texel;  // xy: 1/width, 1/height; zw reserved
	glm::vec4 params; // x: radius (texels); y: bias; z: strength; w reserved
};

static_assert(sizeof(SsaoUniforms) == 32,
              "must match ssao.frag's std140 block");

/// Must match shaders/fxaa.frag (set 3, binding 0).
struct FxaaUniforms {
	glm::vec4 texel; // x: 1/width; y: 1/height; zw reserved
};

static_assert(sizeof(FxaaUniforms) == 16,
              "must match fxaa.frag's std140 block");

/// Must match shaders/sky.vert (set 1, binding 0).
struct SkyVertexUniforms {
	glm::mat4 inv_view_projection;
};

static_assert(sizeof(SkyVertexUniforms) == 64,
              "must match sky.vert's std140 block");

/// Must match shaders/sky.frag (set 3, binding 0).
struct SkyFragmentUniforms {
	glm::vec4 camera_position;
	glm::vec4 sky_color;
	glm::vec4 ground_color;
	glm::vec4 sun_direction;
	glm::vec4 sun_color; // rgb + intensity in .a
	glm::vec4 params;    // x: exposure; y: horizon_warmth; zw reserved
};

static_assert(sizeof(SkyFragmentUniforms) == 96,
              "must match sky.frag's std140 block");

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
		// Albedo (binding 0), shadow map (binding 1), SSAO (binding 2).
		info.num_samplers = 3;
		info.code      = lit_frag_spv;
		info.code_size = lit_frag_spv_len;
	}
	SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
	if (!shader) {
		throw std::runtime_error(SDL_GetError());
	}
	return shader;
}

SDL_GPUShader* create_graphics_shader(SDL_GPUDevice* device,
                                      SDL_GPUShaderStage stage,
                                      const unsigned char* code,
                                      std::size_t code_size,
                                      uint32_t samplers, uint32_t uniforms)
{
	SDL_GPUShaderCreateInfo info{};
	info.format              = SDL_GPU_SHADERFORMAT_SPIRV;
	info.stage               = stage;
	info.entrypoint          = "main";
	info.code                = code;
	info.code_size           = code_size;
	info.num_samplers        = samplers;
	info.num_uniform_buffers = uniforms;
	return SDL_CreateGPUShader(device, &info); // null on failure: caller skips
}

/// Vertex buffer slots of the lit and shadow pipelines: a mesh vertex (0), its
/// UV (1), and one GpuInstance per instance (2).
constexpr uint32_t kVertexBufferSlots = 3;
constexpr uint32_t kVertexAttributes  = 10;

/// Instance 0 of the instance buffer is the identity the immediate stream
/// draws with; the instances of a frame's batches follow it.
constexpr uint32_t kReservedInstances = 1;

/// Initial capacity of the instance buffer, in instances.
constexpr uint32_t kInitialInstances = 1024;

/// Shared vertex input state for the lit and shadow pipelines: both consume
/// the same buffers (shadow.vert reads only the position and the model
/// matrix; unconsumed attributes are legal).
void lit_vertex_descriptions(
        SDL_GPUVertexBufferDescription (&vb)[kVertexBufferSlots],
        SDL_GPUVertexAttribute (&attrs)[kVertexAttributes])
{
	vb[0].slot               = 0;
	vb[0].pitch              = kVertexStride;
	vb[0].input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vb[0].instance_step_rate = 0;
	vb[1].slot               = 1;
	vb[1].pitch              = sizeof(glm::vec2);
	vb[1].input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	vb[1].instance_step_rate = 0;
	vb[2].slot               = 2;
	vb[2].pitch              = sizeof(GpuInstance);
	vb[2].input_rate         = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
	vb[2].instance_step_rate = 0;

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
	attrs[3].location    = 4;
	attrs[3].buffer_slot = 1;
	attrs[3].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attrs[3].offset      = 0;
	attrs[4].location    = 6;
	attrs[4].buffer_slot = 0;
	attrs[4].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
	attrs[4].offset      = offsetof(GpuVertex, roughness);
	// The model matrix is four consecutive vec4 columns: locations 8..11.
	for (uint32_t column = 0; column < 4; ++column) {
		SDL_GPUVertexAttribute& attr = attrs[5 + column];
		attr.location    = 8 + column;
		attr.buffer_slot = 2;
		attr.format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
		attr.offset      = static_cast<Uint32>(offsetof(GpuInstance, model) +
                                                  column * sizeof(glm::vec4));
	}
	// rgb colour multiplier and roughness multiplier, location 12.
	attrs[9].location    = 12;
	attrs[9].buffer_slot = 2;
	attrs[9].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
	attrs[9].offset      = offsetof(GpuInstance, color);
}

} // namespace

GpuRendererSdlGpu::~GpuRendererSdlGpu()
{
	shutdown();
}

void GpuRendererSdlGpu::init(SDL_Window* window, bool debug_device)
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
	ensure_vertex_capacity(kInitialVertexBytes, kInitialVertexBytes / 2u);
	ensure_default_sampler();
	ensure_white_texture();
}

void GpuRendererSdlGpu::init_offscreen(uint32_t width, uint32_t height)
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
	ensure_vertex_capacity(kInitialVertexBytes, kInitialVertexBytes / 2u);
	ensure_default_sampler();
	ensure_white_texture();
}

void GpuRendererSdlGpu::ensure_default_sampler()
{
	if (default_sampler_ || !device_) {
		return;
	}
	SDL_GPUSamplerCreateInfo info{};
	info.min_filter          = SDL_GPU_FILTER_LINEAR;
	info.mag_filter          = SDL_GPU_FILTER_LINEAR;
	info.mipmap_mode         = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	info.address_mode_u      = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	info.address_mode_v      = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	info.address_mode_w      = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	default_sampler_         = SDL_CreateGPUSampler(device_, &info);
	if (!default_sampler_) {
		throw std::runtime_error(SDL_GetError());
	}
}

void GpuRendererSdlGpu::ensure_white_texture()
{
	if (white_texture_ || !device_) {
		return;
	}
	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width                = 1;
	info.height               = 1;
	info.layer_count_or_depth = 1;
	info.num_levels           = 1;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
	white_texture_            = SDL_CreateGPUTexture(device_, &info);
	if (!white_texture_) {
		throw std::runtime_error(SDL_GetError());
	}
	const std::uint8_t pixel[4] = {255, 255, 255, 255};
	SDL_GPUTransferBufferCreateInfo transfer_info{};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size  = sizeof(pixel);
	SDL_GPUTransferBuffer* transfer =
	        SDL_CreateGPUTransferBuffer(device_, &transfer_info);
	if (!transfer) {
		throw std::runtime_error(SDL_GetError());
	}
	void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, true);
	if (!mapped) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		throw std::runtime_error(SDL_GetError());
	}
	std::memcpy(mapped, pixel, sizeof(pixel));
	SDL_UnmapGPUTransferBuffer(device_, transfer);
	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
	if (!cmd) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		throw std::runtime_error(SDL_GetError());
	}
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo src{};
	src.transfer_buffer = transfer;
	src.offset          = 0;
	src.pixels_per_row  = 1;
	src.rows_per_layer  = 1;
	SDL_GPUTextureRegion dst{};
	dst.texture   = white_texture_;
	dst.mip_level = 0;
	dst.layer     = 0;
	dst.x         = 0;
	dst.y         = 0;
	dst.z         = 0;
	dst.w         = 1;
	dst.h         = 1;
	dst.d         = 1;
	SDL_UploadToGPUTexture(copy, &src, &dst, true);
	SDL_EndGPUCopyPass(copy);
	if (!SDL_SubmitGPUCommandBuffer(cmd)) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		throw std::runtime_error(SDL_GetError());
	}
	SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

void GpuRendererSdlGpu::destroy_textures()
{
	for (auto& [key, entry] : albedo_textures_) {
		if (entry.texture) {
			SDL_ReleaseGPUTexture(device_, entry.texture);
			entry.texture = nullptr;
		}
		(void)key;
	}
	albedo_textures_.clear();
	if (white_texture_) {
		SDL_ReleaseGPUTexture(device_, white_texture_);
		white_texture_ = nullptr;
	}
	if (default_sampler_) {
		SDL_ReleaseGPUSampler(device_, default_sampler_);
		default_sampler_ = nullptr;
	}
}

bool GpuRendererSdlGpu::load_texture_from_file(const std::string& path,
                                         const std::string& cache_key)
{
	if (!device_ || path.empty()) {
		return false;
	}
	const auto existing = albedo_textures_.find(cache_key);
	if (existing != albedo_textures_.end() && existing->second.texture) {
		return true;
	}
	int width = 0;
	int height = 0;
	int channels = 0;
	std::uint8_t* pixels =
	        stbi_load(path.c_str(), &width, &height, &channels, 4);
	if (!pixels || width <= 0 || height <= 0) {
		if (pixels) {
			stbi_image_free(pixels);
		}
		return false;
	}

	// Full mip chain so texture() auto-LODs: the sampler already requests
	// LINEAR mip filtering, but a num_levels=1 texture has no mips to pick.
	uint32_t levels = 1;
	for (uint32_t dim =
	             static_cast<uint32_t>(std::max(width, height)) >> 1;
	     dim > 0; dim >>= 1) {
		++levels;
	}
	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	// COLOR_TARGET is required by SDL_GenerateMipmapsForGPUTexture (it
	// blits mip by mip); SAMPLER keeps the texture samplable.
	info.usage = static_cast<SDL_GPUTextureUsageFlags>(
	        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	info.width                = static_cast<Uint32>(width);
	info.height               = static_cast<Uint32>(height);
	info.layer_count_or_depth = 1;
	info.num_levels           = levels;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUTexture* texture   = SDL_CreateGPUTexture(device_, &info);
	if (!texture) {
		stbi_image_free(pixels);
		return false;
	}

	const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
	const std::size_t total     = row_bytes * static_cast<std::size_t>(height);
	SDL_GPUTransferBufferCreateInfo transfer_info{};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size  = total;
	SDL_GPUTransferBuffer* transfer =
	        SDL_CreateGPUTransferBuffer(device_, &transfer_info);
	if (!transfer) {
		SDL_ReleaseGPUTexture(device_, texture);
		stbi_image_free(pixels);
		return false;
	}
	void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, true);
	if (!mapped) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		SDL_ReleaseGPUTexture(device_, texture);
		stbi_image_free(pixels);
		return false;
	}
	std::memcpy(mapped, pixels, total);
	stbi_image_free(pixels);
	SDL_UnmapGPUTransferBuffer(device_, transfer);

	SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
	if (!cmd) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		SDL_ReleaseGPUTexture(device_, texture);
		return false;
	}
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo src{};
	src.transfer_buffer = transfer;
	src.offset          = 0;
	src.pixels_per_row  = static_cast<Uint32>(width);
	src.rows_per_layer  = static_cast<Uint32>(height);
	SDL_GPUTextureRegion dst{};
	dst.texture   = texture;
	dst.mip_level = 0;
	dst.layer     = 0;
	dst.x         = 0;
	dst.y         = 0;
	dst.z         = 0;
	dst.w         = static_cast<Uint32>(width);
	dst.h         = static_cast<Uint32>(height);
	dst.d         = 1;
	SDL_UploadToGPUTexture(copy, &src, &dst, true);
	SDL_EndGPUCopyPass(copy);
	// Blits mip 0 down the chain on the GPU so minified fragments sample
	// filtered mips instead of shimmering on full-res texels.
	SDL_GenerateMipmapsForGPUTexture(cmd, texture);
	const bool submitted = SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(device_, transfer);
	if (!submitted) {
		SDL_ReleaseGPUTexture(device_, texture);
		return false;
	}
	albedo_textures_[cache_key] = CachedAlbedo{texture};
	return true;
}

SDL_GPUTexture* GpuRendererSdlGpu::texture_for_albedo(const std::string& albedo_id,
                                                const AssetRegistry* assets)
{
	ensure_white_texture();
	if (albedo_id.empty()) {
		return white_texture_;
	}
	const auto it = albedo_textures_.find(albedo_id);
	if (it != albedo_textures_.end() && it->second.texture) {
		return it->second.texture;
	}
	std::string path;
	if (assets != nullptr) {
		const std::string* registered = assets->find(albedo_id);
		if (registered != nullptr && !registered->empty()) {
			path = *registered;
		}
	}
	if (path.empty()) {
		return white_texture_;
	}
	if (!load_texture_from_file(path, albedo_id)) {
		return white_texture_;
	}
	return albedo_textures_[albedo_id].texture;
}

void GpuRendererSdlGpu::create_pipeline()
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

	SDL_GPUVertexBufferDescription vb_desc[kVertexBufferSlots]{};
	SDL_GPUVertexAttribute attrs[kVertexAttributes]{};
	lit_vertex_descriptions(vb_desc, attrs);

	SDL_GPUGraphicsPipelineCreateInfo pipe{};
	pipe.vertex_shader   = vert;
	pipe.fragment_shader = frag;
	pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipe.target_info.num_color_targets         = 2u;
	pipe.target_info.color_target_descriptions = color_targets;
	pipe.target_info.has_depth_stencil_target  = true;
	pipe.target_info.depth_stencil_format      = kDepthFormat;
	pipe.vertex_input_state.num_vertex_buffers = kVertexBufferSlots;
	pipe.vertex_input_state.vertex_buffer_descriptions = vb_desc;
	pipe.vertex_input_state.num_vertex_attributes      = kVertexAttributes;
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

void GpuRendererSdlGpu::shadow_light_matrices(glm::mat4& view, glm::mat4& vp) const
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

bool GpuRendererSdlGpu::shadows_ready()
{
	if (!lighting_.shadows_enabled || !device_) {
		return false;
	}
	if (!shadow_support_queried_) {
		shadow_support_queried_ = true;
		shadows_supported_      = SDL_GPUTextureSupportsFormat(
                        device_, kShadowFormat, SDL_GPU_TEXTURETYPE_2D,
                        static_cast<SDL_GPUTextureUsageFlags>(
                                SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET));
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

void GpuRendererSdlGpu::ensure_shadow_resources()
{
	if (!device_ || !shadows_supported_) {
		return;
	}
	const auto disable = [this](const char* what) {
		std::cerr << "shadows: " << what
		          << ", continuing without shadow mapping\n";
		destroy_shadows();
		shadows_supported_ = false;
	};
	if (!shadow_texture_) {
		SDL_GPUTextureCreateInfo info{};
		info.type                 = SDL_GPU_TEXTURETYPE_2D;
		info.format               = kShadowFormat;
		info.usage                = static_cast<SDL_GPUTextureUsageFlags>(
                        SDL_GPU_TEXTUREUSAGE_SAMPLER |
                        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
		info.width                = kShadowMapSize;
		info.height               = kShadowMapSize;
		info.layer_count_or_depth = 1;
		info.num_levels           = 1;
		info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
		shadow_texture_           = SDL_CreateGPUTexture(device_, &info);
		if (!shadow_texture_) {
			disable("shadow map unavailable");
			return;
		}
	}
	if (!shadow_sampler_) {
		// Nearest, clamp-to-edge: lit.frag does its own 3x3 PCF with
		// texelFetch-free texture() taps, so no depth-filtering support is
		// required from the device beyond plain depth sampling.
		SDL_GPUSamplerCreateInfo info{};
		info.min_filter   = SDL_GPU_FILTER_NEAREST;
		info.mag_filter   = SDL_GPU_FILTER_NEAREST;
		info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		shadow_sampler_     = SDL_CreateGPUSampler(device_, &info);
		if (!shadow_sampler_) {
			disable("shadow sampler unavailable");
			return;
		}
	}
	if (!shadow_pipeline_) {
		SDL_GPUShader* vert = create_graphics_shader(
		        device_, SDL_GPU_SHADERSTAGE_VERTEX, shadow_vert_spv,
		        shadow_vert_spv_len, 0, 1);
		SDL_GPUShader* frag = create_graphics_shader(
		        device_, SDL_GPU_SHADERSTAGE_FRAGMENT, shadow_frag_spv,
		        shadow_frag_spv_len, 0, 0);
		if (vert && frag) {
			SDL_GPUVertexBufferDescription vb_desc[kVertexBufferSlots]{};
			SDL_GPUVertexAttribute attrs[kVertexAttributes]{};
			lit_vertex_descriptions(vb_desc, attrs);
			SDL_GPUGraphicsPipelineCreateInfo pipe{};
			pipe.vertex_shader   = vert;
			pipe.fragment_shader = frag;
			pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			pipe.target_info.num_color_targets = 0u;
			pipe.target_info.color_target_descriptions = nullptr;
			pipe.target_info.has_depth_stencil_target  = true;
			pipe.target_info.depth_stencil_format      = kShadowFormat;
			pipe.vertex_input_state.num_vertex_buffers = kVertexBufferSlots;
			pipe.vertex_input_state.vertex_buffer_descriptions = vb_desc;
			pipe.vertex_input_state.num_vertex_attributes      = kVertexAttributes;
			pipe.vertex_input_state.vertex_attributes          = attrs;
			pipe.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			pipe.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
			pipe.depth_stencil_state.enable_depth_test  = true;
			pipe.depth_stencil_state.enable_depth_write = true;
			pipe.depth_stencil_state.compare_op =
			        SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
			// No hardware depth bias: lit.frag carries a constant bias plus
			// a normal offset, which keeps every depth unit on the CPU side.
			shadow_pipeline_ =
			        SDL_CreateGPUGraphicsPipeline(device_, &pipe);
		}
		if (vert) {
			SDL_ReleaseGPUShader(device_, vert);
		}
		if (frag) {
			SDL_ReleaseGPUShader(device_, frag);
		}
		if (!shadow_pipeline_) {
			disable("shadow pipeline unavailable");
			return;
		}
	}
}

void GpuRendererSdlGpu::render_shadow_map()
{
	if (shadow_rendered_ || !cmd_ || geometry_draws_.empty() ||
	    !instance_buffer_) {
		return;
	}
	if (!shadows_ready()) {
		return;
	}
	glm::mat4 light_view{1.0f};
	shadow_light_matrices(light_view, light_vp_);

	SDL_GPUDepthStencilTargetInfo depth{};
	depth.texture          = shadow_texture_;
	depth.clear_depth      = 1.0f;
	depth.load_op          = SDL_GPU_LOADOP_CLEAR;
	depth.store_op         = SDL_GPU_STOREOP_STORE;
	depth.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, nullptr, 0, &depth);
	if (!pass) {
		std::cerr << "shadows: shadow pass failed, "
		             "continuing without shadow mapping\n";
		shadows_supported_ = false;
		return;
	}
	VertexUniforms uniforms{};
	uniforms.view_projection = light_vp_;
	uniforms.view            = light_view;
	SDL_PushGPUVertexUniformData(cmd_, 0, &uniforms, sizeof(uniforms));
	SDL_BindGPUGraphicsPipeline(pass, shadow_pipeline_);
	for (const GeometryDraw& draw : geometry_draws_) {
		record_geometry_draw(pass, draw);
	}
	SDL_EndGPURenderPass(pass);
	shadow_rendered_ = true;
}

void GpuRendererSdlGpu::destroy_shadows()
{
	if (shadow_pipeline_) {
		SDL_ReleaseGPUGraphicsPipeline(device_, shadow_pipeline_);
		shadow_pipeline_ = nullptr;
	}
	if (shadow_texture_) {
		SDL_ReleaseGPUTexture(device_, shadow_texture_);
		shadow_texture_ = nullptr;
	}
	if (shadow_sampler_) {
		SDL_ReleaseGPUSampler(device_, shadow_sampler_);
		shadow_sampler_ = nullptr;
	}
	shadow_rendered_ = false;
	light_vp_        = glm::mat4{1.0f};
}

bool GpuRendererSdlGpu::ensure_fxaa_source(uint32_t width, uint32_t height)
{
	if (!device_ || width == 0 || height == 0 ||
	    swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return false;
	}
	if (fxaa_source_ && fxaa_width_ == width && fxaa_height_ == height) {
		return true;
	}
	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = swapchain_format_;
	info.usage                = static_cast<SDL_GPUTextureUsageFlags>(
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER);
	info.width                = width;
	info.height               = height;
	info.layer_count_or_depth = 1;
	info.num_levels           = 1;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUTexture* replacement = SDL_CreateGPUTexture(device_, &info);
	if (!replacement) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			std::cerr << "fxaa: intermediate target unavailable, "
			             "rendering directly: " << SDL_GetError() << '\n';
		}
		return false;
	}
	if (fxaa_source_) {
		SDL_ReleaseGPUTexture(device_, fxaa_source_);
	}
	fxaa_source_ = replacement;
	fxaa_width_  = width;
	fxaa_height_ = height;
	return true;
}

bool GpuRendererSdlGpu::ensure_fxaa_pipeline()
{
	if (!device_ || swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return false;
	}
	if (fxaa_unavailable_) {
		return false;
	}
	if (fxaa_pipeline_ && fxaa_sampler_ &&
	    fxaa_format_ == swapchain_format_) {
		return true;
	}
	destroy_fxaa();
	SDL_GPUShader* vert = create_graphics_shader(
	        device_, SDL_GPU_SHADERSTAGE_VERTEX, fxaa_vert_spv,
	        fxaa_vert_spv_len, 0, 0);
	SDL_GPUShader* frag = create_graphics_shader(
	        device_, SDL_GPU_SHADERSTAGE_FRAGMENT, fxaa_frag_spv,
	        fxaa_frag_spv_len, 1, 1);
	if (vert && frag) {
		SDL_GPUColorTargetDescription target{};
		target.format = swapchain_format_;
		SDL_GPUGraphicsPipelineCreateInfo pipe{};
		pipe.vertex_shader   = vert;
		pipe.fragment_shader = frag;
		pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		pipe.target_info.num_color_targets         = 1u;
		pipe.target_info.color_target_descriptions = &target;
		pipe.target_info.has_depth_stencil_target  = false;
		pipe.vertex_input_state.num_vertex_buffers = 0;
		pipe.vertex_input_state.vertex_buffer_descriptions = nullptr;
		pipe.vertex_input_state.num_vertex_attributes      = 0;
		pipe.vertex_input_state.vertex_attributes          = nullptr;
		pipe.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		pipe.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		fxaa_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipe);
	}
	if (vert) {
		SDL_ReleaseGPUShader(device_, vert);
	}
	if (frag) {
		SDL_ReleaseGPUShader(device_, frag);
	}
	if (fxaa_pipeline_) {
		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter    = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter    = SDL_GPU_FILTER_LINEAR;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		fxaa_sampler_          = SDL_CreateGPUSampler(device_, &sampler);
	}
	if (!fxaa_pipeline_ || !fxaa_sampler_) {
		std::cerr << "fxaa: post pipeline unavailable, "
		             "rendering directly: " << SDL_GetError() << '\n';
		destroy_fxaa();
		fxaa_unavailable_ = true;
		return false;
	}
	fxaa_format_ = swapchain_format_;
	return true;
}

void GpuRendererSdlGpu::fxaa_resolve(SDL_GPUTexture* destination, uint32_t width,
                               uint32_t height)
{
	if (!cmd_ || !destination || !fxaa_source_ || width == 0 || height == 0 ||
	    !ensure_fxaa_pipeline() || !fxaa_sampler_) {
		return;
	}
	SDL_GPUColorTargetInfo target{};
	target.texture  = destination;
	target.load_op  = SDL_GPU_LOADOP_DONT_CARE;
	target.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &target, 1, nullptr);
	if (!pass) {
		return;
	}
	FxaaUniforms uniforms{};
	uniforms.texel = glm::vec4{1.0f / static_cast<float>(width),
	                           1.0f / static_cast<float>(height), 0.0f, 0.0f};
	SDL_PushGPUFragmentUniformData(cmd_, 0, &uniforms, sizeof(uniforms));
	SDL_BindGPUGraphicsPipeline(pass, fxaa_pipeline_);
	SDL_GPUTextureSamplerBinding binding{};
	binding.texture = fxaa_source_;
	binding.sampler = fxaa_sampler_;
	SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
	// Fullscreen triangle from fxaa.vert; covers the DONT_CARE target fully.
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
}

void GpuRendererSdlGpu::destroy_fxaa()
{
	if (fxaa_pipeline_) {
		SDL_ReleaseGPUGraphicsPipeline(device_, fxaa_pipeline_);
		fxaa_pipeline_ = nullptr;
	}
	if (fxaa_source_) {
		SDL_ReleaseGPUTexture(device_, fxaa_source_);
		fxaa_source_ = nullptr;
	}
	if (fxaa_sampler_) {
		SDL_ReleaseGPUSampler(device_, fxaa_sampler_);
		fxaa_sampler_ = nullptr;
	}
	fxaa_width_      = 0;
	fxaa_height_     = 0;
	fxaa_format_     = SDL_GPU_TEXTUREFORMAT_INVALID;
	pass_redirected_ = false;
	pass_destination_ = nullptr;
}

bool GpuRendererSdlGpu::ensure_sky_pipeline()
{
	if (!device_ || swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return false;
	}
	if (sky_unavailable_) {
		return false;
	}
	if (sky_pipeline_ && sky_format_ == swapchain_format_) {
		return true;
	}
	destroy_sky();
	SDL_GPUShader* vert = create_graphics_shader(
	        device_, SDL_GPU_SHADERSTAGE_VERTEX, sky_vert_spv,
	        sky_vert_spv_len, 0, 1);
	SDL_GPUShader* frag = create_graphics_shader(
	        device_, SDL_GPU_SHADERSTAGE_FRAGMENT, sky_frag_spv,
	        sky_frag_spv_len, 0, 1);
	if (vert && frag) {
		// Same attachments as the lit pipeline: colour target 0 plus the
		// R32F linear-depth target 1 the pyramid reduction reads, with
		// depth. A mismatch with the open scene pass would be a
		// validation error, so the sky pass piggybacks the lit layout.
		SDL_GPUColorTargetDescription targets[2]{};
		targets[0].format = swapchain_format_;
		targets[1].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
		// The sky leaves the linear-depth target alone. Every pass that draws
		// it clears that target to kDepthFar, which is exactly what sky.frag
		// writes, so the write changes nothing; masked off, it is memory
		// traffic saved on every sky pixel.
		targets[1].blend_state.enable_color_write_mask = true;
		targets[1].blend_state.color_write_mask        = 0;
		SDL_GPUGraphicsPipelineCreateInfo pipe{};
		pipe.vertex_shader   = vert;
		pipe.fragment_shader = frag;
		pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		pipe.target_info.num_color_targets         = 2u;
		pipe.target_info.color_target_descriptions = targets;
		pipe.target_info.has_depth_stencil_target  = true;
		pipe.target_info.depth_stencil_format      = kDepthFormat;
		pipe.vertex_input_state.num_vertex_buffers = 0;
		pipe.vertex_input_state.vertex_buffer_descriptions = nullptr;
		pipe.vertex_input_state.num_vertex_attributes      = 0;
		pipe.vertex_input_state.vertex_attributes          = nullptr;
		pipe.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		pipe.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		// Drawn last, at depth 1.0, over what no geometry reached (see
		// kGeometryDepthMax): the test passes only on pixels still holding the
		// cleared 1.0. It writes no depth: it is the last draw of the pass, and
		// writing 1.0 over 1.0 changes nothing.
		pipe.depth_stencil_state.enable_depth_test  = true;
		pipe.depth_stencil_state.enable_depth_write = false;
		pipe.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		sky_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipe);
	}
	if (vert) {
		SDL_ReleaseGPUShader(device_, vert);
	}
	if (frag) {
		SDL_ReleaseGPUShader(device_, frag);
	}
	if (!sky_pipeline_) {
		std::cerr << "sky: background pipeline unavailable, "
		             "continuing with the clear color: " << SDL_GetError()
		          << '\n';
		destroy_sky();
		sky_unavailable_ = true;
		return false;
	}
	sky_format_ = swapchain_format_;
	return true;
}

void GpuRendererSdlGpu::draw_sky(const glm::mat4& view_projection,
                           const glm::vec3& camera_position)
{
	// Once per pass, after the geometry of the first draw() that has any:
	// over a pixel only where nothing was drawn, so geometry is never shaded
	// twice. A pass with no draws stays a clear (e.g. the editor's clear-only
	// window pass). Disabled restores the exact legacy clear-color path.
	if (sky_drawn_ || !pass_ || !lighting_.sky_enabled) {
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

	SDL_PushGPUVertexUniformData(cmd_, 0, &vertex_uniforms,
	                             sizeof(vertex_uniforms));
	SDL_PushGPUFragmentUniformData(cmd_, 0, &fragment_uniforms,
	                               sizeof(fragment_uniforms));

	// The geometry just drawn used the lower half of the depth range; the sky
	// sits at the far end of the full one, above all of it.
	set_pass_viewport(1.0f);

	SDL_BindGPUGraphicsPipeline(pass_, sky_pipeline_);
	// Bufferless fullscreen triangle from sky.vert; covers both targets.
	SDL_DrawGPUPrimitives(pass_, 3, 1, 0, 0);
}

void GpuRendererSdlGpu::set_pass_viewport(float max_depth)
{
	SDL_GPUViewport viewport{};
	viewport.w         = static_cast<float>(pass_width_);
	viewport.h         = static_cast<float>(pass_height_);
	viewport.min_depth = 0.0f;
	viewport.max_depth = max_depth;
	SDL_SetGPUViewport(pass_, &viewport);
}

void GpuRendererSdlGpu::destroy_sky()
{
	if (sky_pipeline_) {
		SDL_ReleaseGPUGraphicsPipeline(device_, sky_pipeline_);
		sky_pipeline_ = nullptr;
	}
	sky_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
}

void GpuRendererSdlGpu::set_blob_casters(const glm::vec4* spheres, uint32_t count)
{
	blob_count_ = 0;
	if (spheres == nullptr || count == 0) {
		return;
	}
	blob_count_ = std::min(count, pbr::kMaxBlobCasters);
	for (uint32_t i = 0; i < blob_count_; ++i) {
		blob_spheres_[i] = spheres[i];
	}
}

void GpuRendererSdlGpu::clear_blob_casters()
{
	blob_count_ = 0;
}

bool GpuRendererSdlGpu::ensure_ssao_resources(uint32_t width, uint32_t height)
{
	if (!device_ || width == 0 || height == 0 ||
	    swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return false;
	}
	// Quarter-res AO: resolution-independent UVs keep it valid at any size.
	const uint32_t w = std::max(1u, width / 4u);
	const uint32_t h = std::max(1u, height / 4u);
	if (ssao_texture_ && ssao_width_ == w && ssao_height_ == h) {
		return true;
	}
	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage                = static_cast<SDL_GPUTextureUsageFlags>(
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                SDL_GPU_TEXTUREUSAGE_SAMPLER);
	info.width                = w;
	info.height               = h;
	info.layer_count_or_depth = 1;
	info.num_levels           = 1;
	info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUTexture* replacement = SDL_CreateGPUTexture(device_, &info);
	if (!replacement) {
		if (!ssao_unavailable_) {
			ssao_unavailable_ = true;
			std::cerr << "ssao: AO target unavailable, "
			             "continuing without AO: " << SDL_GetError()
			          << '\n';
		}
		return false;
	}
	if (ssao_texture_) {
		SDL_ReleaseGPUTexture(device_, ssao_texture_);
	}
	ssao_texture_ = replacement;
	ssao_width_   = w;
	ssao_height_  = h;
	return true;
}

bool GpuRendererSdlGpu::ensure_ssao_pipeline()
{
	if (!device_ || swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return false;
	}
	if (ssao_unavailable_) {
		return false;
	}
	if (ssao_pipeline_ && ssao_sampler_ && ssao_depth_sampler_) {
		return true;
	}
	destroy_ssao();
	ssao_unavailable_ = false;
	SDL_GPUShader* vert = create_graphics_shader(
	        device_, SDL_GPU_SHADERSTAGE_VERTEX, ssao_vert_spv,
	        ssao_vert_spv_len, 0, 0);
	SDL_GPUShader* frag = create_graphics_shader(
	        device_, SDL_GPU_SHADERSTAGE_FRAGMENT, ssao_frag_spv,
	        ssao_frag_spv_len, 1, 1);
	if (vert && frag) {
		SDL_GPUColorTargetDescription target{};
		target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		SDL_GPUGraphicsPipelineCreateInfo pipe{};
		pipe.vertex_shader   = vert;
		pipe.fragment_shader = frag;
		pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		pipe.target_info.num_color_targets         = 1u;
		pipe.target_info.color_target_descriptions = &target;
		pipe.target_info.has_depth_stencil_target  = false;
		pipe.vertex_input_state.num_vertex_buffers = 0;
		pipe.vertex_input_state.vertex_buffer_descriptions = nullptr;
		pipe.vertex_input_state.num_vertex_attributes      = 0;
		pipe.vertex_input_state.vertex_attributes          = nullptr;
		pipe.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		pipe.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		ssao_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipe);
	}
	if (vert) {
		SDL_ReleaseGPUShader(device_, vert);
	}
	if (frag) {
		SDL_ReleaseGPUShader(device_, frag);
	}
	if (ssao_pipeline_) {
		SDL_GPUSamplerCreateInfo linear{};
		linear.min_filter    = SDL_GPU_FILTER_LINEAR;
		linear.mag_filter    = SDL_GPU_FILTER_LINEAR;
		linear.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		linear.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		linear.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		ssao_sampler_ = SDL_CreateGPUSampler(device_, &linear);
		SDL_GPUSamplerCreateInfo nearest{};
		// Nearest: the depth source is R32_FLOAT, and float-linear
		// filtering is not universally supported (lavapipe included).
		// Taps are exact texel reads; the 12-tap average smooths.
		nearest.min_filter    = SDL_GPU_FILTER_NEAREST;
		nearest.mag_filter    = SDL_GPU_FILTER_NEAREST;
		nearest.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		nearest.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		nearest.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		ssao_depth_sampler_ = SDL_CreateGPUSampler(device_, &nearest);
	}
	if (!ssao_pipeline_ || !ssao_sampler_ || !ssao_depth_sampler_) {
		std::cerr << "ssao: post pipeline unavailable, "
		             "continuing without AO: " << SDL_GetError() << '\n';
		destroy_ssao();
		ssao_unavailable_ = true;
		return false;
	}
	return true;
}

void GpuRendererSdlGpu::run_ssao_pass(uint32_t width, uint32_t height)
{
	if (!cmd_ || !pyramid_texture_ || width == 0 || height == 0 ||
	    !ensure_ssao_pipeline() ||
	    !ensure_ssao_resources(width, height)) {
		return;
	}
	SDL_GPUColorTargetInfo target{};
	target.texture  = ssao_texture_;
	target.load_op  = SDL_GPU_LOADOP_CLEAR;
	target.store_op = SDL_GPU_STOREOP_STORE;
	target.clear_color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &target, 1, nullptr);
	if (!pass) {
		return;
	}
	SsaoUniforms uniforms{};
	uniforms.texel =
	        glm::vec4{1.0f / static_cast<float>(width),
	                  1.0f / static_cast<float>(height), 0.0f, 0.0f};
	uniforms.params = glm::vec4{2.5f, 0.02f, 1.0f, 0.0f};
	SDL_PushGPUFragmentUniformData(cmd_, 0, &uniforms, sizeof(uniforms));
	SDL_BindGPUGraphicsPipeline(pass, ssao_pipeline_);
	SDL_GPUTextureSamplerBinding binding{};
	binding.texture = pyramid_texture_;
	binding.sampler = ssao_depth_sampler_;
	SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
	SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	SDL_EndGPURenderPass(pass);
	ssao_target_width_  = width;
	ssao_target_height_ = height;
}

void GpuRendererSdlGpu::destroy_ssao()
{
	if (ssao_pipeline_) {
		SDL_ReleaseGPUGraphicsPipeline(device_, ssao_pipeline_);
		ssao_pipeline_ = nullptr;
	}
	if (ssao_texture_) {
		SDL_ReleaseGPUTexture(device_, ssao_texture_);
		ssao_texture_ = nullptr;
	}
	if (ssao_sampler_) {
		SDL_ReleaseGPUSampler(device_, ssao_sampler_);
		ssao_sampler_ = nullptr;
	}
	if (ssao_depth_sampler_) {
		SDL_ReleaseGPUSampler(device_, ssao_depth_sampler_);
		ssao_depth_sampler_ = nullptr;
	}
	ssao_width_          = 0;
	ssao_height_         = 0;
	ssao_target_width_   = 0;
	ssao_target_height_  = 0;
}

void GpuRendererSdlGpu::destroy_buffers()
{
	if (vertex_buffer_) {
		SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
		vertex_buffer_ = nullptr;
	}
	if (transfer_buffer_) {
		SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
		transfer_buffer_ = nullptr;
	}
	if (uv_buffer_) {
		SDL_ReleaseGPUBuffer(device_, uv_buffer_);
		uv_buffer_ = nullptr;
	}
	if (uv_transfer_buffer_) {
		SDL_ReleaseGPUTransferBuffer(device_, uv_transfer_buffer_);
		uv_transfer_buffer_ = nullptr;
	}
	capacity_    = 0;
	uv_capacity_ = 0;
}

void GpuRendererSdlGpu::ensure_vertex_capacity(uint32_t vertex_bytes, uint32_t uv_bytes)
{
	if (vertex_bytes <= capacity_ && uv_bytes <= uv_capacity_) {
		return;
	}
	uint32_t next = capacity_ ? capacity_ : kInitialVertexBytes;
	while (next < vertex_bytes) {
		next *= 2;
	}
	uint32_t uv_next = uv_capacity_ ? uv_capacity_ : kInitialVertexBytes / 2u;
	while (uv_next < uv_bytes) {
		uv_next *= 2;
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

	SDL_GPUBufferCreateInfo uvb{};
	uvb.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	uvb.size  = uv_next;
	uv_buffer_ = SDL_CreateGPUBuffer(device_, &uvb);
	if (!uv_buffer_) {
		throw std::runtime_error(SDL_GetError());
	}

	SDL_GPUTransferBufferCreateInfo utb{};
	utb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	utb.size  = uv_next;
	uv_transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &utb);
	if (!uv_transfer_buffer_) {
		throw std::runtime_error(SDL_GetError());
	}
	capacity_    = next;
	uv_capacity_ = uv_next;
}

bool GpuRendererSdlGpu::create_occlusion_pipeline()
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
void GpuRendererSdlGpu::destroy_pyramid()
{
	// A pyramid download still in flight is simply forgotten: its frame's
	// fence belongs to frame_fences_ and is reaped like any other, and SDL
	// keeps the transfer buffer released below alive until the GPU is done.
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

void GpuRendererSdlGpu::destroy_occlusion()
{
	destroy_pyramid();
	if (hiz_pipeline_) {
		SDL_ReleaseGPUComputePipeline(device_, hiz_pipeline_);
		hiz_pipeline_ = nullptr;
	}
	occlusion_enabled_ = false;
}

void GpuRendererSdlGpu::reap_frame_fences()
{
	// Query, never wait. A frame that has not finished stays queued and is
	// retried next time; nothing here may stall the loop.
	while (!frame_fences_.empty() &&
	       SDL_QueryGPUFence(device_, frame_fences_.front().fence)) {
		SDL_ReleaseGPUFence(device_, frame_fences_.front().fence);
		completed_serial_ = frame_fences_.front().serial;
		frame_fences_.pop_front();
	}
}

void GpuRendererSdlGpu::wait_for_frame_slot()
{
	reap_frame_fences();
	while (frame_fences_.size() >= kFramesInFlight) {
		SDL_GPUFence* const oldest = frame_fences_.front().fence;
		SDL_WaitForGPUFences(device_, true, &oldest, 1);
		SDL_ReleaseGPUFence(device_, oldest);
		completed_serial_ = frame_fences_.front().serial;
		frame_fences_.pop_front();
	}
}

void GpuRendererSdlGpu::release_frame_fences()
{
	// Only called with the GPU idle, so every fence is signalled and this
	// cannot block.
	for (const FrameFence& frame : frame_fences_) {
		SDL_ReleaseGPUFence(device_, frame.fence);
	}
	frame_fences_.clear();
	completed_serial_ = submit_serial_;
}

void GpuRendererSdlGpu::ensure_pyramid(uint32_t width, uint32_t height)
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
void GpuRendererSdlGpu::ensure_pyramid_stand_in(uint32_t width, uint32_t height)
{
	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	// SAMPLER keeps the SSAO pass valid on this path too (it samples mip 0).
	info.usage = static_cast<SDL_GPUTextureUsageFlags>(
	        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
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

void GpuRendererSdlGpu::ensure_pyramid_texture(uint32_t width, uint32_t height)
{
	// Only mips 0..K exist: the query reads mip K and nothing coarser, so
	// reducing further and transferring more would be pure waste.
	const uint32_t levels = kDepthPyramidMinLevel + 1;

	SDL_GPUTextureCreateInfo info{};
	info.type                 = SDL_GPU_TEXTURETYPE_2D;
	info.format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	// SIMULTANEOUS_READ_WRITE, never READ | WRITE: depth.comp reads and writes
	// storage images, and SDL_gpu.h warns the two are not equivalent.
	// SAMPLER lets the SSAO pass sample mip 0 as the fullscreen AO source;
	// without it the SSAO sampler binding would be a validation error.
	info.usage = static_cast<SDL_GPUTextureUsageFlags>(
	        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
	        SDL_GPU_TEXTUREUSAGE_SAMPLER |
	        SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
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

void GpuRendererSdlGpu::poll_pyramid_fence()
{
	// Collects the frames that have finished. Polls, never waits.
	reap_frame_fences();
	occlusion_ready_ = false;
	if (!pyramid_in_flight_) {
		return;
	}
	if (completed_serial_ < pyramid_serial_) {
		return; // still in flight: skip occlusion this frame, never block
	}
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
			read_mapped_floats(texels.data() + static_cast<std::size_t>(y) * w,
			                   row, w);
		}
	}
	SDL_UnmapGPUTransferBuffer(device_, pyramid_transfer_);

	// adopt() rejects a chain whose transferred mips disagree with the
	// accessors and leaves the pyramid empty, so readiness is read back off
	// the object rather than assumed. Reporting ready after a rejection is the
	// false reassurance an empty pyramid is worst at: an empty pyramid never
	// occludes, so the failure is invisible in the culling counts.
	occlusion_pyramid_.adopt(pyramid_width_, pyramid_height_, pyramid_levels_,
	                         std::move(mips));
	occlusion_ready_ = !occlusion_pyramid_.empty();
	occlusion_aspect_ =
	        occlusion_ready_ && occlusion_pyramid_.height() > 0
	                ? static_cast<float>(occlusion_pyramid_.width()) /
	                          static_cast<float>(occlusion_pyramid_.height())
	                : 0.0f;
}

void GpuRendererSdlGpu::record_pyramid()
{
	// Gated on occlusion_enabled_, not merely on hiz_pipeline_: the pyramid
	// texture always exists because the scene pass writes it as color target 1,
	// so without this the reduction would still run pointlessly whenever
	// occlusion was disabled.
	//
	// pyramid_in_flight_ keeps this to at most one outstanding download.
	// Recording a second one into the same transfer buffer while the first
	// submission is still pending would overwrite the bytes the poll is going
	// to map. Skipping the download costs one frame of occlusion, which is the
	// direction this project calls safe: the frame still renders, it just
	// culls nothing.
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

void GpuRendererSdlGpu::ensure_depth_target(uint32_t width, uint32_t height)
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

void GpuRendererSdlGpu::begin_frame_offscreen()
{
	if (!offscreen_ || !offscreen_color_) {
		return;
	}
	wait_for_frame_slot();
	begin_geometry_frame();
	poll_pyramid_fence();
	cmd_ = SDL_AcquireGPUCommandBuffer(device_);
	if (!cmd_) {
		throw std::runtime_error(SDL_GetError());
	}
	swapchain_ = offscreen_color_;
	ensure_depth_target(offscreen_width_, offscreen_height_);
	shadow_rendered_  = false;
	pass_redirected_  = false;
	pass_destination_ = nullptr;
}

std::vector<uint8_t> GpuRendererSdlGpu::capture_rgba(uint32_t& width, uint32_t& height)
{
	width  = 0;
	height = 0;
	if (!offscreen_ || !offscreen_color_) {
		return {};
	}
	width  = offscreen_width_;
	height = offscreen_height_;
	return download_rgba(offscreen_color_, width, height);
}

std::vector<uint8_t> GpuRendererSdlGpu::capture_viewport_rgba(uint32_t& width,
                                                        uint32_t& height)
{
	width  = 0;
	height = 0;
	if (!device_ || !viewport_texture_) {
		return {};
	}
	width  = viewport_width_;
	height = viewport_height_;
	return download_rgba(viewport_texture_, width, height);
}

std::vector<uint8_t> GpuRendererSdlGpu::download_rgba(SDL_GPUTexture* texture,
                                                uint32_t width, uint32_t height)
{
	SDL_WaitForGPUIdle(device_);

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
	region.texture = texture;
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

bool GpuRendererSdlGpu::resize_offscreen(uint32_t width, uint32_t height)
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

bool GpuRendererSdlGpu::resize_viewport(uint32_t width, uint32_t height)
{
	if (!device_ || width == 0 || height == 0 ||
	    swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
		return false;
	}
	if (width == viewport_width_ && height == viewport_height_ &&
	    viewport_texture_) {
		return true;
	}

	// Create new textures before releasing old ones, so a failure leaves the
	// renderer on its old viewport rather than on none.
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

	SDL_GPUTextureCreateInfo depth{};
	depth.type                 = SDL_GPU_TEXTURETYPE_2D;
	depth.format               = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	depth.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
	depth.width                = width;
	depth.height               = height;
	depth.layer_count_or_depth = 1;
	depth.num_levels           = 1;
	depth.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	// R32F stand-in for colour target 1 (lit.frag writes linear depth there).
	// Sized to the panel, not the swapchain, so the viewport pass never
	// distorts the pyramid's aspect ratio.
	SDL_GPUTextureCreateInfo pyramid{};
	pyramid.type                 = SDL_GPU_TEXTURETYPE_2D;
	pyramid.format               = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	pyramid.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	pyramid.width                = width;
	pyramid.height               = height;
	pyramid.layer_count_or_depth = 1;
	pyramid.num_levels           = 1;
	pyramid.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	SDL_GPUTexture* new_color   = SDL_CreateGPUTexture(device_, &color);
	SDL_GPUTexture* new_depth   = SDL_CreateGPUTexture(device_, &depth);
	SDL_GPUTexture* new_pyramid = SDL_CreateGPUTexture(device_, &pyramid);
	if (!new_color || !new_depth || !new_pyramid) {
		if (new_color) {
			SDL_ReleaseGPUTexture(device_, new_color);
		}
		if (new_depth) {
			SDL_ReleaseGPUTexture(device_, new_depth);
		}
		if (new_pyramid) {
			SDL_ReleaseGPUTexture(device_, new_pyramid);
		}
		return false;
	}

	if (viewport_texture_) {
		SDL_ReleaseGPUTexture(device_, viewport_texture_);
	}
	if (viewport_depth_) {
		SDL_ReleaseGPUTexture(device_, viewport_depth_);
	}
	if (viewport_pyramid_) {
		SDL_ReleaseGPUTexture(device_, viewport_pyramid_);
	}
	viewport_texture_  = new_color;
	viewport_depth_    = new_depth;
	viewport_pyramid_  = new_pyramid;
	viewport_width_    = width;
	viewport_height_   = height;
	return true;
}

bool GpuRendererSdlGpu::begin_viewport_pass()
{
	if (!cmd_ || !viewport_texture_ || !viewport_pyramid_ || !viewport_depth_) {
		return false;
	}
	// Same once-per-frame shadow map the scene pass uses; a no-op when the
	// scene pass already rendered it.
	render_shadow_map();
	SDL_GPUTexture* color_target = viewport_texture_;
	pass_redirected_             = false;
	pass_destination_            = nullptr;
	sky_drawn_                   = false;
	if (lighting_.fxaa_enabled && !fxaa_unavailable_ && viewport_width_ != 0 &&
	    viewport_height_ != 0 && ensure_fxaa_pipeline() &&
	    ensure_fxaa_source(viewport_width_, viewport_height_)) {
		color_target      = fxaa_source_;
		pass_redirected_  = true;
		pass_destination_ = viewport_texture_;
	}
	SDL_GPUColorTargetInfo targets[2]{};
	targets[0].texture     = color_target;
	targets[0].clear_color = SDL_FColor{clear_r_, clear_g_, clear_b_, 1.0f};
	targets[0].load_op     = SDL_GPU_LOADOP_CLEAR;
	targets[0].store_op    = SDL_GPU_STOREOP_STORE;
	targets[1].texture     = viewport_pyramid_;
	targets[1].mip_level   = 0;
	targets[1].clear_color = SDL_FColor{kDepthFar, kDepthFar, kDepthFar, kDepthFar};
	targets[1].load_op     = SDL_GPU_LOADOP_CLEAR;
	targets[1].store_op    = SDL_GPU_STOREOP_STORE;

	SDL_GPUDepthStencilTargetInfo depth{};
	depth.texture          = viewport_depth_;
	depth.clear_depth      = 1.0f;
	depth.load_op          = SDL_GPU_LOADOP_CLEAR;
	depth.store_op         = SDL_GPU_STOREOP_DONT_CARE;
	depth.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
	depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

	// The size SDL gives the pass's default viewport: that of its first colour
	// target, which is the viewport texture or the FXAA source made from it.
	pass_width_  = viewport_width_;
	pass_height_ = viewport_height_;
	pass_ = SDL_BeginGPURenderPass(cmd_, targets, 2, &depth);
	if (!pass_) {
		pass_redirected_  = false;
		pass_destination_ = nullptr;
	}
	return pass_ != nullptr;
}

void GpuRendererSdlGpu::end_viewport_pass()
{
	if (pass_) {
		SDL_EndGPURenderPass(pass_);
		pass_ = nullptr;
		// FXAA resolve onto the viewport texture ImGui samples. Skipped when
		// the pass rendered directly (pass_destination_ null).
		if (pass_redirected_ && pass_destination_) {
			fxaa_resolve(pass_destination_, fxaa_width_, fxaa_height_);
		}
		pass_redirected_  = false;
		pass_destination_ = nullptr;
		// The viewport pass does NOT record the Hi-Z pyramid. The main pass
		// (begin_scene_pass / end_scene_pass) stays authoritative for occlusion.
	}
}

bool GpuRendererSdlGpu::write_ppm(const std::string& path, std::string* error)
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

void GpuRendererSdlGpu::shutdown()
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
	// Every frame fence is now signalled, so this only releases handles; it
	// cannot block. Releasing them here rather than leaking keeps SDL's fence
	// pool and the Vulkan device in step through SDL_DestroyGPUDevice.
	release_frame_fences();

	abandon_geometry_staging();
	destroy_buffers();
	destroy_instance_buffers();
	destroy_meshes();
	destroy_shadows();
	destroy_fxaa();
	destroy_sky();
	destroy_ssao();
	destroy_textures();
	if (depth_texture_) {
		SDL_ReleaseGPUTexture(device_, depth_texture_);
		depth_texture_ = nullptr;
	}
	if (offscreen_color_) {
		SDL_ReleaseGPUTexture(device_, offscreen_color_);
		offscreen_color_ = nullptr;
	}
	if (viewport_texture_) {
		SDL_ReleaseGPUTexture(device_, viewport_texture_);
		viewport_texture_ = nullptr;
	}
	if (viewport_depth_) {
		SDL_ReleaseGPUTexture(device_, viewport_depth_);
		viewport_depth_ = nullptr;
	}
	if (viewport_pyramid_) {
		SDL_ReleaseGPUTexture(device_, viewport_pyramid_);
		viewport_pyramid_ = nullptr;
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

void GpuRendererSdlGpu::begin_frame(SDL_Window* window, float clear_r, float clear_g,
                              float clear_b)
{
	clear_r_ = clear_r;
	clear_g_ = clear_g;
	clear_b_ = clear_b;

	begin_geometry_frame();

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
	swapchain_width_  = width;
	swapchain_height_ = height;
	// The shadow map renders once per frame (first scene/viewport pass wins);
	// a new command buffer means it must render again.
	shadow_rendered_  = false;
	pass_redirected_  = false;
	pass_destination_ = nullptr;
}

void GpuRendererSdlGpu::begin_geometry_frame()
{
	abandon_geometry_staging();
	// The draw list belongs to the frame that uploaded it: a frame that
	// uploads nothing draws no shadows, and cannot reach a mesh the eviction
	// below has just released.
	geometry_draws_.clear();
	// Cheap enough to check every 64th frame; a scene that stopped drawing
	// still lets its meshes age out.
	if ((frames_rendered_ & 63u) == 0 && !mesh_ids_.empty()) {
		evict_idle_meshes();
	}
}

bool GpuRendererSdlGpu::mesh_live(GpuMeshId mesh) const
{
	return mesh < meshes_.size() && meshes_[mesh].live;
}

uint32_t GpuRendererSdlGpu::mesh_index_count(GpuMeshId mesh) const
{
	return mesh_live(mesh) ? meshes_[mesh].index_count : 0u;
}

uint32_t GpuRendererSdlGpu::cached_mesh_count() const
{
	return static_cast<uint32_t>(mesh_ids_.size());
}

void GpuRendererSdlGpu::release_mesh(GpuMeshId mesh)
{
	if (!mesh_live(mesh)) {
		return;
	}
	CachedMesh& cached = meshes_[mesh];
	// SDL frees a buffer only once the work using it has finished, so a mesh
	// released right after the frame that drew it is safe.
	if (cached.vertex) {
		SDL_ReleaseGPUBuffer(device_, cached.vertex);
	}
	if (cached.uv) {
		SDL_ReleaseGPUBuffer(device_, cached.uv);
	}
	if (cached.index) {
		SDL_ReleaseGPUBuffer(device_, cached.index);
	}
	mesh_ids_.erase(cached.key);
	std::erase_if(pending_meshes_, [mesh](const PendingMeshUpload& pending) {
		return pending.id == mesh;
	});
	cached = CachedMesh{};
	free_mesh_ids_.push_back(mesh);
}

void GpuRendererSdlGpu::evict_idle_meshes()
{
	for (GpuMeshId id = 0; id < meshes_.size(); ++id) {
		const CachedMesh& cached = meshes_[id];
		if (cached.live && frames_rendered_ - cached.last_used > kMeshIdleFrames) {
			release_mesh(id);
		}
	}
}

void GpuRendererSdlGpu::destroy_meshes()
{
	for (GpuMeshId id = 0; id < meshes_.size(); ++id) {
		release_mesh(id);
	}
	meshes_.clear();
	free_mesh_ids_.clear();
	mesh_ids_.clear();
	pending_meshes_.clear();
}

GpuMeshId GpuRendererSdlGpu::acquire_mesh(const std::string& key,
                                    const GpuMeshData& data)
{
	if (!device_ || data.positions == nullptr || data.indices == nullptr ||
	    data.vertex_count == 0 || data.index_count == 0) {
		return kNoGpuMesh;
	}
	if (const auto found = mesh_ids_.find(key); found != mesh_ids_.end()) {
		const GpuMeshId cached_id = found->second;
		CachedMesh& cached        = meshes_[cached_id];
		if (cached.vertex_count == data.vertex_count &&
		    cached.source_index_count == data.index_count) {
			cached.last_used = frames_rendered_;
			return cached_id;
		}
		// The key now names geometry of a different size: replace it.
		release_mesh(cached_id);
	}

	const std::size_t vertex_count = data.vertex_count;
	PendingMeshUpload upload;
	upload.vertices.resize(vertex_count);
	for (std::size_t i = 0; i < vertex_count; ++i) {
		GpuVertex& vertex = upload.vertices[i];
		vertex.position   = data.positions[i];
		// A vertex without a normal is stored as zero; lit.vert turns that
		// into +Z, as the CPU path always did.
		vertex.normal = (data.normals != nullptr && i < data.normal_count)
		                        ? data.normals[i]
		                        : glm::vec3{0.0f};
		// The instance supplies colour and roughness; 1 passes them through.
		vertex.color     = glm::vec3{1.0f};
		vertex.roughness = 1.0f;
	}
	upload.uvs.resize(vertex_count);
	const bool has_uvs = data.uvs != nullptr && data.uv_count == vertex_count;
	for (std::size_t i = 0; i < vertex_count; ++i) {
		upload.uvs[i] = has_uvs ? data.uvs[i] : glm::vec2{0.0f};
	}
	// An index that names no vertex is skipped, exactly as the world-space
	// path always did, so both paths draw the same triangles.
	upload.indices.reserve(data.index_count);
	for (std::size_t i = 0; i < data.index_count; ++i) {
		if (data.indices[i] < vertex_count) {
			upload.indices.push_back(data.indices[i]);
		}
	}
	if (upload.indices.empty()) {
		return kNoGpuMesh;
	}

	const auto create = [this](SDL_GPUBufferUsageFlags usage, std::size_t bytes) {
		SDL_GPUBufferCreateInfo info{};
		info.usage = usage;
		info.size  = static_cast<Uint32>(bytes);
		return SDL_CreateGPUBuffer(device_, &info);
	};
	CachedMesh mesh;
	mesh.vertex = create(SDL_GPU_BUFFERUSAGE_VERTEX,
	                     vertex_count * sizeof(GpuVertex));
	mesh.uv     = create(SDL_GPU_BUFFERUSAGE_VERTEX,
	                     vertex_count * sizeof(glm::vec2));
	mesh.index  = create(SDL_GPU_BUFFERUSAGE_INDEX,
	                     upload.indices.size() * sizeof(uint32_t));
	if (!mesh.vertex || !mesh.uv || !mesh.index) {
		const std::string error = SDL_GetError();
		for (SDL_GPUBuffer* buffer : {mesh.vertex, mesh.uv, mesh.index}) {
			if (buffer) {
				SDL_ReleaseGPUBuffer(device_, buffer);
			}
		}
		throw std::runtime_error(error);
	}
	mesh.vertex_count       = static_cast<uint32_t>(vertex_count);
	mesh.source_index_count = static_cast<uint32_t>(data.index_count);
	mesh.index_count        = static_cast<uint32_t>(upload.indices.size());
	mesh.last_used          = frames_rendered_;
	mesh.key                = key;
	mesh.live               = true;

	GpuMeshId id = kNoGpuMesh;
	if (!free_mesh_ids_.empty()) {
		id = free_mesh_ids_.back();
		free_mesh_ids_.pop_back();
	} else {
		id = static_cast<GpuMeshId>(meshes_.size());
		meshes_.emplace_back();
	}
	meshes_[id] = std::move(mesh);
	mesh_ids_.emplace(key, id);
	upload.id = id;
	pending_meshes_.push_back(std::move(upload));
	return id;
}

void GpuRendererSdlGpu::upload_pending_meshes(SDL_GPUCopyPass* copy)
{
	if (pending_meshes_.empty()) {
		return;
	}
	std::size_t total = 0;
	for (const PendingMeshUpload& pending : pending_meshes_) {
		total += pending.vertices.size() * sizeof(GpuVertex) +
		         pending.uvs.size() * sizeof(glm::vec2) +
		         pending.indices.size() * sizeof(uint32_t);
	}
	SDL_GPUTransferBufferCreateInfo info{};
	info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	info.size  = static_cast<Uint32>(total);
	SDL_GPUTransferBuffer* transfer =
	        SDL_CreateGPUTransferBuffer(device_, &info);
	if (!transfer) {
		throw std::runtime_error(SDL_GetError());
	}
	auto* bytes = static_cast<uint8_t*>(
	        SDL_MapGPUTransferBuffer(device_, transfer, false));
	if (!bytes) {
		const std::string error = SDL_GetError();
		SDL_ReleaseGPUTransferBuffer(device_, transfer);
		throw std::runtime_error(error);
	}
	std::size_t offset = 0;
	const auto put     = [&](const void* source, std::size_t size) {
		std::memcpy(bytes + offset, source, size);
		offset += size;
	};
	for (const PendingMeshUpload& pending : pending_meshes_) {
		put(pending.vertices.data(), pending.vertices.size() * sizeof(GpuVertex));
		put(pending.uvs.data(), pending.uvs.size() * sizeof(glm::vec2));
		put(pending.indices.data(), pending.indices.size() * sizeof(uint32_t));
	}
	SDL_UnmapGPUTransferBuffer(device_, transfer);

	offset = 0;
	const auto record = [&](SDL_GPUBuffer* destination, std::size_t size) {
		SDL_GPUTransferBufferLocation source{};
		source.transfer_buffer = transfer;
		source.offset          = static_cast<Uint32>(offset);
		SDL_GPUBufferRegion region{};
		region.buffer = destination;
		region.offset = 0;
		region.size   = static_cast<Uint32>(size);
		SDL_UploadToGPUBuffer(copy, &source, &region, false);
		offset += size;
	};
	for (const PendingMeshUpload& pending : pending_meshes_) {
		const CachedMesh& mesh = meshes_[pending.id];
		record(mesh.vertex, pending.vertices.size() * sizeof(GpuVertex));
		record(mesh.uv, pending.uvs.size() * sizeof(glm::vec2));
		record(mesh.index, pending.indices.size() * sizeof(uint32_t));
	}
	// SDL keeps the transfer buffer alive until the copy has executed.
	SDL_ReleaseGPUTransferBuffer(device_, transfer);
	pending_meshes_.clear();
}

void GpuRendererSdlGpu::ensure_instance_capacity(uint32_t bytes)
{
	if (instance_buffer_ && instance_transfer_ && bytes <= instance_capacity_) {
		return;
	}
	uint32_t next = instance_capacity_
	                        ? instance_capacity_
	                        : kInitialInstances * static_cast<uint32_t>(
	                                                      sizeof(GpuInstance));
	while (next < bytes) {
		next *= 2;
	}
	destroy_instance_buffers();

	SDL_GPUBufferCreateInfo buffer_info{};
	buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	buffer_info.size  = next;
	instance_buffer_  = SDL_CreateGPUBuffer(device_, &buffer_info);
	if (!instance_buffer_) {
		throw std::runtime_error(SDL_GetError());
	}
	SDL_GPUTransferBufferCreateInfo transfer_info{};
	transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer_info.size  = next;
	instance_transfer_  = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
	if (!instance_transfer_) {
		throw std::runtime_error(SDL_GetError());
	}
	instance_capacity_ = next;
}

void GpuRendererSdlGpu::destroy_instance_buffers()
{
	if (instance_buffer_) {
		SDL_ReleaseGPUBuffer(device_, instance_buffer_);
		instance_buffer_ = nullptr;
	}
	if (instance_transfer_) {
		SDL_ReleaseGPUTransferBuffer(device_, instance_transfer_);
		instance_transfer_ = nullptr;
	}
	instance_capacity_ = 0;
}

void GpuRendererSdlGpu::abandon_geometry_staging()
{
	if (!geometry_mapped_) {
		return;
	}
	if (staged_vertices_ != 0) {
		SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);
		SDL_UnmapGPUTransferBuffer(device_, uv_transfer_buffer_);
	}
	SDL_UnmapGPUTransferBuffer(device_, instance_transfer_);
	geometry_mapped_  = false;
	staged_vertices_  = 0;
	staged_instances_ = 0;
}

GpuRendererSdlGpu::GeometryStaging GpuRendererSdlGpu::begin_geometry_upload(
        uint32_t immediate_vertices, uint32_t instance_count)
{
	abandon_geometry_staging();
	geometry_draws_.clear();
	if (!cmd_ || (immediate_vertices == 0 && instance_count == 0)) {
		return {};
	}

	GeometryStaging staging;
	if (immediate_vertices != 0) {
		ensure_vertex_capacity(
		        immediate_vertices * static_cast<uint32_t>(sizeof(GpuVertex)),
		        immediate_vertices * static_cast<uint32_t>(sizeof(glm::vec2)));
		void* vertices =
		        SDL_MapGPUTransferBuffer(device_, transfer_buffer_, true);
		if (!vertices) {
			throw std::runtime_error(SDL_GetError());
		}
		void* uvs =
		        SDL_MapGPUTransferBuffer(device_, uv_transfer_buffer_, true);
		if (!uvs) {
			const std::string error = SDL_GetError();
			SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);
			throw std::runtime_error(error);
		}
		staging.vertices = static_cast<GpuVertex*>(vertices);
		staging.uvs      = static_cast<glm::vec2*>(uvs);
	}

	ensure_instance_capacity((instance_count + kReservedInstances) *
	                         static_cast<uint32_t>(sizeof(GpuInstance)));
	void* instances =
	        SDL_MapGPUTransferBuffer(device_, instance_transfer_, true);
	if (!instances) {
		const std::string error = SDL_GetError();
		if (immediate_vertices != 0) {
			SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);
			SDL_UnmapGPUTransferBuffer(device_, uv_transfer_buffer_);
		}
		throw std::runtime_error(error);
	}
	auto* records = static_cast<GpuInstance*>(instances);
	records[0]    = GpuInstance{}; // the identity the immediate stream draws with
	staging.instances = records + kReservedInstances;

	staged_vertices_  = immediate_vertices;
	staged_instances_ = instance_count;
	geometry_mapped_  = true;
	return staging;
}

void GpuRendererSdlGpu::commit_geometry_upload(const GpuDrawBatch* batches,
                                         uint32_t batch_count)
{
	if (!geometry_mapped_) {
		return;
	}
	const uint32_t vertices  = staged_vertices_;
	const uint32_t instances = staged_instances_;
	if (vertices != 0) {
		SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);
		SDL_UnmapGPUTransferBuffer(device_, uv_transfer_buffer_);
	}
	SDL_UnmapGPUTransferBuffer(device_, instance_transfer_);
	geometry_mapped_  = false;
	staged_vertices_  = 0;
	staged_instances_ = 0;

	geometry_draws_.clear();
	for (uint32_t i = 0; i < batch_count; ++i) {
		const GpuDrawBatch& batch = batches[i];
		const bool instanced      = batch.mesh != kNoGpuMesh;
		if (instanced ? (batch.instance_count == 0 || !mesh_live(batch.mesh))
		              : batch.vertex_count == 0) {
			continue;
		}
		geometry_draws_.push_back(GeometryDraw{
		        batch.mesh, batch.first_vertex, batch.vertex_count,
		        batch.first_instance, batch.instance_count});
	}

	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd_);
	if (vertices != 0) {
		SDL_GPUTransferBufferLocation source{};
		source.transfer_buffer = transfer_buffer_;
		SDL_GPUBufferRegion region{};
		region.buffer = vertex_buffer_;
		region.size   = vertices * static_cast<uint32_t>(sizeof(GpuVertex));
		SDL_UploadToGPUBuffer(copy, &source, &region, true);

		SDL_GPUTransferBufferLocation uv_source{};
		uv_source.transfer_buffer = uv_transfer_buffer_;
		SDL_GPUBufferRegion uv_region{};
		uv_region.buffer = uv_buffer_;
		uv_region.size   = vertices * static_cast<uint32_t>(sizeof(glm::vec2));
		SDL_UploadToGPUBuffer(copy, &uv_source, &uv_region, true);
	}
	SDL_GPUTransferBufferLocation instance_source{};
	instance_source.transfer_buffer = instance_transfer_;
	SDL_GPUBufferRegion instance_region{};
	instance_region.buffer = instance_buffer_;
	instance_region.size   = (instances + kReservedInstances) *
	                         static_cast<uint32_t>(sizeof(GpuInstance));
	SDL_UploadToGPUBuffer(copy, &instance_source, &instance_region, true);
	upload_pending_meshes(copy);
	SDL_EndGPUCopyPass(copy);
}

void GpuRendererSdlGpu::record_geometry_draw(SDL_GPURenderPass* pass,
                                       const GeometryDraw& draw)
{
	SDL_GPUBufferBinding bindings[kVertexBufferSlots]{};
	bindings[2].buffer = instance_buffer_;
	if (draw.mesh == kNoGpuMesh) {
		bindings[0].buffer = vertex_buffer_;
		bindings[1].buffer = uv_buffer_;
		SDL_BindGPUVertexBuffers(pass, 0, bindings, kVertexBufferSlots);
		// Instance 0 is the identity: the stream is already in world space.
		SDL_DrawGPUPrimitives(pass, draw.vertex_count, 1, draw.first_vertex, 0);
		return;
	}
	if (!mesh_live(draw.mesh)) {
		return;
	}
	const CachedMesh& mesh = meshes_[draw.mesh];
	bindings[0].buffer     = mesh.vertex;
	bindings[1].buffer     = mesh.uv;
	SDL_BindGPUVertexBuffers(pass, 0, bindings, kVertexBufferSlots);
	SDL_GPUBufferBinding index{};
	index.buffer = mesh.index;
	SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
	SDL_DrawGPUIndexedPrimitives(pass, mesh.index_count, draw.instance_count, 0,
	                             0, draw.first_instance + kReservedInstances);
}

void GpuRendererSdlGpu::begin_scene_pass()
{
	if (!cmd_ || !swapchain_) {
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
	const uint32_t target_width =
	        offscreen_ ? offscreen_width_ : swapchain_width_;
	const uint32_t target_height =
	        offscreen_ ? offscreen_height_ : swapchain_height_;
	SDL_GPUTexture* color_target = swapchain_;
	pass_redirected_             = false;
	pass_destination_            = nullptr;
	sky_drawn_                   = false;
	if (record_occlusion_ && lighting_.fxaa_enabled && !fxaa_unavailable_ &&
	    target_width != 0 && target_height != 0 && ensure_fxaa_pipeline() &&
	    ensure_fxaa_source(target_width, target_height)) {
		color_target      = fxaa_source_;
		pass_redirected_  = true;
		pass_destination_ = swapchain_;
	}
	SDL_GPUColorTargetInfo targets[2]{};
	targets[0].texture     = color_target;
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

	// The size SDL gives the pass's default viewport: that of its first colour
	// target, which is the swapchain or the FXAA source made from it.
	pass_width_  = target_width;
	pass_height_ = target_height;
	pass_ = SDL_BeginGPURenderPass(cmd_, targets, target_count,
	                               depth_texture_ ? &depth : nullptr);
	if (!pass_) {
		throw std::runtime_error(SDL_GetError());
	}
}

void GpuRendererSdlGpu::bind_lit_state(const glm::mat4& view,
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
	                       shadow_rendered_ && shadow_texture_ &&
	                       shadow_sampler_;
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
	// binds white so the factor reads 1.
	const bool ssao_on = lighting_.ssao_enabled && !ssao_unavailable_ &&
	                     ssao_texture_ != nullptr && ssao_sampler_ != nullptr &&
	                     ssao_target_width_ != 0 && ssao_target_height_ != 0;
	fragment_uniforms.ssao_params =
	        glm::vec4{ssao_on ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
	fragment_uniforms.ssao_texel =
	        ssao_on ? glm::vec4{1.0f / static_cast<float>(ssao_target_width_),
	                            1.0f / static_cast<float>(ssao_target_height_),
	                            0.0f, 0.0f}
	                : glm::vec4{0.0f};

	SDL_PushGPUVertexUniformData(cmd_, 0, &vertex_uniforms,
	                             sizeof(vertex_uniforms));
	SDL_PushGPUFragmentUniformData(cmd_, 0, &fragment_uniforms,
	                               sizeof(fragment_uniforms));

	SDL_BindGPUGraphicsPipeline(pass_, pipeline_);
	// Binding 1 (shadow map) lives here, next to the pipeline, so every draw
	// samples a valid descriptor: the map when lit, else the white texel
	// (depth 1.0 reads as lit, failing open). Binding 0 (albedo) is bound per
	// run of batches by draw(). Binding 2 (SSAO) samples the previous
	// frame's AO texture, or white (AO=1) when off/unavailable.
	SDL_GPUTextureSamplerBinding shadow_binding{};
	if (shadow_on) {
		shadow_binding.texture = shadow_texture_;
		shadow_binding.sampler = shadow_sampler_;
	} else {
		shadow_binding.texture = white_texture_;
		shadow_binding.sampler = default_sampler_;
	}
	SDL_BindGPUFragmentSamplers(pass_, 1, &shadow_binding, 1);
	SDL_GPUTextureSamplerBinding ssao_binding{};
	if (ssao_on) {
		ssao_binding.texture = ssao_texture_;
		ssao_binding.sampler = ssao_sampler_;
	} else {
		ssao_binding.texture = white_texture_;
		ssao_binding.sampler = default_sampler_;
	}
	SDL_BindGPUFragmentSamplers(pass_, 2, &ssao_binding, 1);
}

void GpuRendererSdlGpu::draw(const glm::mat4& view, const glm::mat4& view_projection,
                       const glm::vec3& camera_position,
                       const GpuDrawBatch* batches, uint32_t batch_count,
                       const AssetRegistry* assets)
{
	if (!pass_ || !pipeline_ || batches == nullptr || batch_count == 0 ||
	    !vertex_buffer_ || !uv_buffer_ || !instance_buffer_) {
		return;
	}
	ensure_default_sampler();
	ensure_white_texture();

	bool state_bound             = false;
	SDL_GPUTexture* bound_albedo = nullptr;
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
			set_pass_viewport(kGeometryDepthMax);
			bind_lit_state(view, view_projection, camera_position);
			state_bound = true;
		}
		SDL_GPUTexture* albedo = texture_for_albedo(batch.albedo_id, assets);
		if (albedo != bound_albedo) {
			SDL_GPUTextureSamplerBinding binding{};
			binding.texture = albedo;
			binding.sampler = default_sampler_;
			SDL_BindGPUFragmentSamplers(pass_, 0, &binding, 1);
			bound_albedo = albedo;
		}
		record_geometry_draw(
		        pass_, GeometryDraw{batch.mesh, batch.first_vertex,
		                            batch.vertex_count, batch.first_instance,
		                            batch.instance_count});
	}

	// Background step for this pass, on every target (swapchain, viewport,
	// offscreen): the sky dome, once, over the pixels no geometry covers. It
	// goes after the geometry so that its fragment shader never runs where a
	// surface already stands. A pass with no draws keeps the legacy clear
	// colour.
	if (state_bound) {
		draw_sky(view_projection, camera_position);
	}
}

void GpuRendererSdlGpu::end_scene_pass()
{
	if (pass_) {
		SDL_EndGPURenderPass(pass_);
		pass_ = nullptr;
		// FXAA resolve onto the real target. The pyramid reduction reads
		// target 1, which is never redirected, so resolve order vs. the
		// reduction below does not matter.
		if (pass_redirected_ && pass_destination_) {
			fxaa_resolve(pass_destination_, fxaa_width_, fxaa_height_);
		}
		pass_redirected_  = false;
		pass_destination_ = nullptr;
		// SSAO samples the linear depth this pass just wrote (colour target
		// 1) and produces the AO texture the NEXT frame's lit pass samples:
		// one frame of lag, no circular dependency. Disabled restores the
		// legacy path (lit binds white, AO=1).
		if (lighting_.ssao_enabled && !ssao_unavailable_ &&
		    pyramid_texture_) {
			const uint32_t target_width =
			        offscreen_ ? offscreen_width_ : swapchain_width_;
			const uint32_t target_height =
			        offscreen_ ? offscreen_height_ : swapchain_height_;
			if (target_width != 0 && target_height != 0) {
				run_ssao_pass(target_width, target_height);
			}
		}
		// The reduction reads the colour target this pass just wrote, so it can
		// only be recorded after the render pass has ended. A clear-only pass
		// behind the editor suppresses this so it does not replace the pyramid
		// with an empty window-sized image.
		if (record_occlusion_) {
			record_pyramid();
		}
		record_occlusion_ = true;
	}
}

void GpuRendererSdlGpu::end_frame()
{
	end_scene_pass();
	if (cmd_) {
		// Captured before cmd_ is cleared: the submit still needs the buffer,
		// and cmd_ must not dangle if a submit throws.
		SDL_GPUCommandBuffer* const submit = cmd_;
		const bool had_pyramid_download    = pyramid_recorded_;

		// Offscreen frames always carry a fence, which begin_frame_offscreen()
		// throttles on; a windowed frame needs one only for its pyramid
		// download, the swapchain paces the rest. frame_fences_ owns the fence
		// until it is reaped.
		pyramid_recorded_ = false;
		cmd_              = nullptr;
		++frames_rendered_;

		const uint64_t serial = ++submit_serial_;
		if (had_pyramid_download || offscreen_) {
			SDL_GPUFence* fence =
			        SDL_SubmitGPUCommandBufferAndAcquireFence(submit);
			if (!fence) {
				// SDL's Vulkan backend has already ended this command buffer
				// inside the failed call, so there is nothing left to fall back
				// to: re-submitting would submit an already-ended buffer. Report
				// it the way every other submit failure in this file is
				// reported.
				throw std::runtime_error(SDL_GetError());
			}
			frame_fences_.push_back(FrameFence{serial, fence});
			if (had_pyramid_download) {
				pyramid_serial_    = serial;
				pyramid_in_flight_ = true;
			}
		} else if (!SDL_SubmitGPUCommandBuffer(submit)) {
			throw std::runtime_error(SDL_GetError());
		}
	}
	swapchain_ = nullptr;
}

const char* GpuRendererSdlGpu::driver_name() const
{
	return device_ ? SDL_GetGPUDeviceDriver(device_) : "";
}

void GpuRendererSdlGpu::wait_idle()
{
	if (device_) {
		SDL_WaitForGPUIdle(device_);
		release_frame_fences();
	}
}

const char* GpuRendererSdlGpu::device_name() const
{
	return device_ ? SDL_GetStringProperty(SDL_GetGPUDeviceProperties(device_),
	                                       SDL_PROP_GPU_DEVICE_NAME_STRING, "?")
	               : "";
}

const char* GpuRendererSdlGpu::device_driver() const
{
	return device_ ? SDL_GetStringProperty(
	                         SDL_GetGPUDeviceProperties(device_),
	                         SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "?")
	               : "";
}

bool GpuRendererSdlGpu::init_ui_backend()
{
	if (!device_) {
		return false;
	}
	ImGui_ImplSDLGPU3_InitInfo info{};
	info.Device            = device_;
	info.ColorTargetFormat = swapchain_format_;
	info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
	return ImGui_ImplSDLGPU3_Init(&info);
}

void GpuRendererSdlGpu::shutdown_ui_backend()
{
	ImGui_ImplSDLGPU3_Shutdown();
}

void GpuRendererSdlGpu::ui_backend_new_frame()
{
	ImGui_ImplSDLGPU3_NewFrame();
}

void GpuRendererSdlGpu::render_ui(ImDrawData* draw_data)
{
	if (!draw_data || draw_data->CmdListsCount == 0 || !cmd_ || !swapchain_) {
		return;
	}
	ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd_);

	SDL_GPUColorTargetInfo target{};
	target.texture  = swapchain_;
	target.load_op  = SDL_GPU_LOADOP_LOAD;
	target.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &target, 1, nullptr);
	ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd_, pass);
	SDL_EndGPURenderPass(pass);
}

uint64_t GpuRendererSdlGpu::viewport_ui_texture() const
{
	return reinterpret_cast<uint64_t>(viewport_texture_);
}

} // namespace lotus
