// Deterministic offscreen render benchmark (driven by autoresearch.sh).
//
// Builds a fixed synthetic scene (seeded, no I/O except generated textures),
// renders it through the full offscreen pipeline one frame at a time and
// waits for the GPU to go idle after every frame. The wait is deliberate:
// occlusion culling reads the previous frame's Hi-Z pyramid only when its
// fence has signalled, so an un-synchronised loop would cull a timing-
// dependent set of entities and measure a different workload every run.
//
// Each frame is split into phases so a regression can be attributed:
//
//   begin   begin_frame_offscreen(): fence poll, pyramid unpack, cmd buffer
//   build   Scene::upload(): cull, LOD, CPU vertex transform, upload copy
//   record  begin_scene_pass() .. end_scene_pass(): shadow, lit, FXAA, SSAO,
//           Hi-Z passes recorded
//   submit  end_frame(): vkQueueSubmit
//   gpu     wait_idle(): execution of the submitted work
//
// The camera orbit and the moving props are pure functions of the frame
// index, so warmup + measured frames replay identically on every run.
//
// --pipelined switches to throughput: frames go out back to back with only the
// renderer's own frames-in-flight limit between them, for --seconds of wall
// time, and the report is frames per second (and vertices per second) rather
// than per-phase medians. --no-occlusion pins the culling, because with frames
// in flight the Hi-Z history a frame culls against depends on timing.
//
// Exit codes follow the engine CLI: 0 ok, 1 runtime failure, 2 CLI misuse.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/assets.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"
#include "stb_image.h"
#include "stb_image_write.h"

using namespace lotus;

namespace {

constexpr const char* kUsage =
        "usage: lotus_render_bench [--width W] [--height H] [--warmup N]\n"
        "                          [--frames N] [--rounds N] [--seed S]\n"
        "                          [--grid N] [--lods N] [--assets-dir DIR]\n"
        "                          [--golden PATH] [--write-golden PATH]\n"
        "                          [--ppm PATH] [--pipelined] [--seconds S]\n"
        "                          [--no-occlusion] [--frame-hashes N]";

constexpr int kTextureCount          = 16;
constexpr uint32_t kTextureSize      = 512;
constexpr uint32_t kThumbnailDivisor = 4;

struct Options {
	uint32_t width{1280};
	uint32_t height{720};
	/// Frames rendered (and discarded) before timing starts: lazy pipeline
	/// creation, JIT compilation (lavapipe), texture uploads, pyramid warm-up.
	/// They are the tail of the camera loop, so timing starts mid-motion.
	int warmup{12};
	/// Timed frames per round; also the length of the camera loop.
	int frames{40};
	/// Rounds of timed frames in one process. Every round replays the same
	/// camera loop, so rounds do identical work; the reported value of each
	/// phase is the median over all rounds' frames.
	int rounds{3};
	uint64_t seed{42};
	/// Props are laid out on a grid x grid lattice.
	int grid{28};
	int lods{160};
	std::string assets_dir{"bench_assets"};
	/// Golden thumbnail to compare the fidelity frame against.
	std::string golden;
	/// Writes the golden thumbnail + hash of the fidelity frame.
	std::string write_golden;
	/// Writes the fidelity frame as a binary PPM for eyeballing.
	std::string ppm;
	/// Throughput mode: frames go out back to back, waiting for the GPU only
	/// through the renderer's frames-in-flight limit, for `seconds` of wall
	/// time; the report is frames per second instead of per-phase medians.
	bool pipelined{false};
	double seconds{3.0};
	/// Hi-Z occlusion culling (the engine's normal behaviour). Off pins the
	/// workload: with frames in flight the culling history a frame sees
	/// depends on timing.
	bool occlusion{true};
	/// Per-frame verification: renders this many frames of the standard camera
	/// loop, capturing and hashing each (see --frame-hashes).
	int frame_hashes{0};
};

bool parse_int(const std::string& text, long long& out)
{
	try {
		std::size_t used = 0;
		out              = std::stoll(text, &used);
		return used == text.size();
	} catch (const std::exception&) {
		return false;
	}
}

bool parse_options(int argc, char** argv, Options& options)
{
	for (int i = 1; i < argc; ++i) {
		const std::string flag = argv[i];
		if (flag == "--pipelined") {
			options.pipelined = true;
			continue;
		}
		if (flag == "--no-occlusion") {
			options.occlusion = false;
			continue;
		}
		if (i + 1 >= argc) {
			std::cerr << "lotus_render_bench: flag '" << flag
			          << "' needs a value\n";
			return false;
		}
		const std::string value = argv[++i];
		long long number        = 0;
		const bool numeric      = parse_int(value, number);
		const auto need_number  = [&]() {
			if (!numeric || number < 0) {
				std::cerr << "lotus_render_bench: flag '" << flag
				          << "' needs a non-negative integer, got '"
				          << value << "'\n";
				return false;
			}
			return true;
		};
		if (flag == "--width") {
			if (!need_number()) {
				return false;
			}
			options.width = static_cast<uint32_t>(number);
		} else if (flag == "--height") {
			if (!need_number()) {
				return false;
			}
			options.height = static_cast<uint32_t>(number);
		} else if (flag == "--warmup") {
			if (!need_number()) {
				return false;
			}
			options.warmup = static_cast<int>(number);
		} else if (flag == "--frames") {
			if (!need_number()) {
				return false;
			}
			options.frames = static_cast<int>(number);
		} else if (flag == "--rounds") {
			if (!need_number()) {
				return false;
			}
			options.rounds = static_cast<int>(number);
		} else if (flag == "--seed") {
			if (!need_number()) {
				return false;
			}
			options.seed = static_cast<uint64_t>(number);
		} else if (flag == "--grid") {
			if (!need_number()) {
				return false;
			}
			options.grid = static_cast<int>(number);
		} else if (flag == "--lods") {
			if (!need_number()) {
				return false;
			}
			options.lods = static_cast<int>(number);
		} else if (flag == "--seconds") {
			char* end       = nullptr;
			options.seconds = std::strtod(value.c_str(), &end);
			if (end == value.c_str() || *end != '\0' || options.seconds <= 0.0) {
				std::cerr << "lotus_render_bench: flag '--seconds' needs a "
				             "positive number, got '"
				          << value << "'\n";
				return false;
			}
		} else if (flag == "--frame-hashes") {
			if (!need_number()) {
				return false;
			}
			options.frame_hashes = static_cast<int>(number);
		} else if (flag == "--assets-dir") {
			options.assets_dir = value;
		} else if (flag == "--golden") {
			options.golden = value;
		} else if (flag == "--write-golden") {
			options.write_golden = value;
		} else if (flag == "--ppm") {
			options.ppm = value;
		} else {
			std::cerr << "lotus_render_bench: unknown flag '" << flag << "'\n";
			return false;
		}
	}
	if (options.width == 0 || options.height == 0 || options.frames <= 0 ||
	    options.rounds <= 0 || options.grid <= 0) {
		std::cerr << "lotus_render_bench: width, height, frames, rounds and "
		             "grid must be non-zero\n";
		return false;
	}
	return true;
}

/// xorshift64*: identical on every platform, unlike std::uniform_*.
class Rng {
public:
	explicit Rng(uint64_t seed)
	    : state_{seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull}
	{
		if (state_ == 0) {
			state_ = 0x9E3779B97F4A7C15ull;
		}
	}

	uint32_t next()
	{
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		return static_cast<uint32_t>((state_ * 0x2545F4914F6CDD1Dull) >> 32);
	}

	/// Uniform in [0, 1).
	float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
	float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
	uint32_t below(uint32_t n) { return next() % n; }

private:
	uint64_t state_;
};

uint32_t hash2(uint32_t x, uint32_t y, uint32_t salt)
{
	uint32_t h = x * 374761393u + y * 668265263u + salt * 2246822519u;
	h          = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

/// Writes kTextureCount procedural textures (checker, stripes, bricks, blocky
/// noise) as uncompressed BMPs and registers them as bench_tex_NN. BMP so the
/// harness does not spend its setup time in zlib.
bool write_textures(const std::string& dir, AssetRegistry& assets)
{
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec) {
		std::cerr << "lotus_render_bench: cannot create " << dir << ": "
		          << ec.message() << "\n";
		return false;
	}
	std::vector<uint8_t> pixels(static_cast<std::size_t>(kTextureSize) *
	                            kTextureSize * 3u);
	for (int k = 0; k < kTextureCount; ++k) {
		const uint32_t salt = static_cast<uint32_t>(k) * 7919u + 17u;
		const float tint[3] = {
		        0.45f + 0.55f * static_cast<float>(hash2(1, 0, salt) & 255) / 255.0f,
		        0.45f + 0.55f * static_cast<float>(hash2(2, 0, salt) & 255) / 255.0f,
		        0.45f + 0.55f * static_cast<float>(hash2(3, 0, salt) & 255) / 255.0f};
		const uint32_t cell = 8u << (static_cast<uint32_t>(k) % 4u);
		for (uint32_t y = 0; y < kTextureSize; ++y) {
			for (uint32_t x = 0; x < kTextureSize; ++x) {
				float v = 0.0f;
				switch (k % 4) {
				case 0: // checker
					v = ((x / cell + y / cell) & 1u) ? 1.0f : 0.35f;
					break;
				case 1: // stripes
					v = ((x / cell) & 1u) ? 1.0f : 0.5f;
					break;
				case 2: { // bricks
					const uint32_t row    = y / cell;
					const uint32_t offset = (row & 1u) ? cell : 0u;
					const bool mortar =
					        (y % cell) < 2u || ((x + offset) % (cell * 2u)) < 2u;
					v = mortar ? 0.3f : 0.9f;
					break;
				}
				default: // blocky value noise, two octaves
					v = 0.5f * static_cast<float>(
					                   hash2(x / cell, y / cell, salt) & 255) /
					            255.0f +
					    0.5f * static_cast<float>(
					                   hash2(x / 4u, y / 4u, salt + 1u) & 255) /
					            255.0f;
					break;
				}
				const std::size_t at =
				        (static_cast<std::size_t>(y) * kTextureSize + x) * 3u;
				for (int c = 0; c < 3; ++c) {
					const float scaled = tint[c] * (0.55f + 0.45f * v);
					pixels[at + static_cast<std::size_t>(c)] =
					        static_cast<uint8_t>(std::min(scaled, 1.0f) * 255.0f);
				}
			}
		}
		char name[32];
		std::snprintf(name, sizeof(name), "bench_tex_%02d", k);
		const std::string path = dir + "/" + name + ".bmp";
		if (!stbi_write_bmp(path.c_str(), static_cast<int>(kTextureSize),
		                    static_cast<int>(kTextureSize), 3, pixels.data())) {
			std::cerr << "lotus_render_bench: cannot write " << path << "\n";
			return false;
		}
		assets.set(name, path);
	}
	return true;
}

struct Mover {
	Entity entity;
	glm::vec3 base_position;
	float base_yaw;
	float phase;
};

struct World {
	Entity camera{NullEntity};
	std::vector<Mover> movers;
	std::size_t entities{0};
};

constexpr const char* kPropMeshes[] = {
        "builtin:cube",     "builtin:sphere", "builtin:cylinder",
        "builtin:cone",     "builtin:pyramid", "builtin:torus"};

std::string texture_id(uint32_t k)
{
	char name[32];
	std::snprintf(name, sizeof(name), "bench_tex_%02u", k);
	return name;
}

/// A town of props on a lattice (a fifth tall enough to occlude what stands
/// behind them), a textured ground plane, and LOD entities scattered through
/// it. A sixth of the props move every frame.
World build_world(Scene& scene, const Options& options)
{
	Rng rng{options.seed};
	World world;
	Registry& registry = scene.registry();

	world.camera = scene.create("camera");
	registry.emplace<Camera3D>(world.camera, Camera3D{});

	const Entity ground = scene.create("ground");
	registry.emplace<MeshSource>(ground, MeshSource{"builtin:plane", ""});
	registry.emplace<Material>(
	        ground, Material{{0.55f, 0.55f, 0.57f}, texture_id(0), 0.9f});
	registry.get<Transform>(ground).scale = {240.0f, 1.0f, 240.0f};

	const float spacing = 4.0f;
	const float half    = static_cast<float>(options.grid - 1) * 0.5f;
	const glm::vec3 palette[] = {
	        {0.78f, 0.32f, 0.22f}, {0.22f, 0.47f, 0.55f},
	        {0.86f, 0.71f, 0.50f}, {0.55f, 0.55f, 0.57f},
	        {0.35f, 0.62f, 0.30f}, {0.62f, 0.38f, 0.62f},
	        {0.90f, 0.85f, 0.30f}, {0.30f, 0.35f, 0.70f}};
	const auto pick_color = [&]() {
		const glm::vec3 base = palette[rng.below(8)];
		return glm::clamp(base + glm::vec3{rng.range(-0.08f, 0.08f)}, 0.0f,
		                  1.0f);
	};
	const auto pick_albedo = [&]() {
		return rng.below(100) < 40 ? texture_id(rng.below(kTextureCount))
		                           : std::string{};
	};

	int index = 0;
	for (int gz = 0; gz < options.grid; ++gz) {
		for (int gx = 0; gx < options.grid; ++gx, ++index) {
			const Entity entity = scene.create("prop_" + std::to_string(index));
			Transform& transform = registry.get<Transform>(entity);
			const bool tall      = rng.below(100) < 20;
			if (tall) {
				transform.scale = {rng.range(1.5f, 3.0f), rng.range(4.0f, 14.0f),
				                   rng.range(1.5f, 3.0f)};
				registry.emplace<MeshSource>(entity,
				                             MeshSource{"builtin:cube", ""});
			} else {
				const float s   = rng.range(0.6f, 1.6f);
				transform.scale = {s, s * rng.range(0.8f, 1.5f), s};
				registry.emplace<MeshSource>(
				        entity, MeshSource{kPropMeshes[rng.below(6)], ""});
			}
			transform.position = {
			        (static_cast<float>(gx) - half) * spacing + rng.range(-0.8f, 0.8f),
			        transform.scale.y * 0.5f,
			        (static_cast<float>(gz) - half) * spacing + rng.range(-0.8f, 0.8f)};
			transform.rotation = {0.0f, rng.range(0.0f, 6.2831853f), 0.0f};
			registry.emplace<Material>(
			        entity, Material{pick_color(), pick_albedo(),
			                         rng.range(0.15f, 0.95f)});
			if (index % 6 == 0) {
				world.movers.push_back(Mover{entity, transform.position,
				                             transform.rotation.y,
				                             rng.range(0.0f, 6.2831853f)});
			}
		}
	}

	const float extent = half * spacing;
	for (int i = 0; i < options.lods; ++i) {
		const Entity entity = scene.create("lod_" + std::to_string(i));
		Transform& transform = registry.get<Transform>(entity);
		const float s        = rng.range(0.8f, 1.8f);
		transform.scale      = {s, s, s};
		transform.position   = {rng.range(-extent, extent), s * 0.75f,
		                        rng.range(-extent, extent)};
		registry.emplace<Lod>(
		        entity,
		        Lod{{LodLevel{"builtin:sphere", 30.0f},
		             LodLevel{"builtin:cylinder", 60.0f},
		             LodLevel{"builtin:cube", 110.0f}}});
		registry.emplace<Material>(
		        entity, Material{pick_color(), pick_albedo(),
		                         rng.range(0.15f, 0.95f)});
	}
	world.entities = registry.storage<Entity>().size();
	return world;
}

/// Pure function of \p u in [0, 1), the position within one timed round: the
/// camera sweeps out and back along an arc of the orbit (a triangle wave, so
/// the path is continuous when a round repeats) and the movers bob and spin a
/// whole number of turns. Only world matrices of root entities are touched,
/// so no hierarchy propagation is needed (and none is timed).
void animate(Scene& scene, World& world, float u)
{
	Registry& registry   = scene.registry();
	constexpr float kTau = 6.2831853f;
	const float sweep    = u < 0.5f ? 2.0f * u : 2.0f * (1.0f - u);
	{
		const float angle  = 0.9f + sweep * 0.5f;
		Camera3D& camera   = registry.get<Camera3D>(world.camera);
		camera.position    = {std::cos(angle) * 46.0f, 19.0f,
		                      std::sin(angle) * 46.0f};
		camera.target      = {0.0f, 2.0f, 0.0f};
		camera.fov_degrees = 55.0f;
		camera.near_plane  = 0.1f;
		camera.far_plane   = 300.0f;
	}
	for (const Mover& mover : world.movers) {
		Transform& transform = registry.get<Transform>(mover.entity);
		transform.position   = mover.base_position;
		transform.position.y += 0.4f * std::sin(2.0f * kTau * u + mover.phase);
		transform.rotation.y = mover.base_yaw + kTau * u;
		transform.world      = transform.local_matrix();
	}
}

/// Matches the showcase scene's look (scenes/main.json) with every feature on,
/// so the whole pipeline is exercised.
void configure_lighting(GpuRenderer& gpu)
{
	LightingSettings& lighting = gpu.lighting();
	lighting.direction         = {-0.62f, 0.35f, 0.42f};
	lighting.color             = {1.0f, 0.82f, 0.6f};
	lighting.intensity         = 1.15f;
	lighting.ambient           = {0.14f, 0.13f, 0.16f};
	lighting.exposure          = 1.0f;
	lighting.sky_color         = {0.32f, 0.45f, 0.68f};
	lighting.ground_color      = {0.3f, 0.24f, 0.18f};
	lighting.shadows_enabled   = true;
	lighting.fxaa_enabled      = true;
	lighting.sky_enabled       = true;
	lighting.horizon_warmth    = 0.9f;
	lighting.fog_enabled       = true;
	lighting.bounce_enabled    = true;
	lighting.ssao_enabled      = true;
}

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point from, Clock::time_point to)
{
	return std::chrono::duration<double, std::milli>(to - from).count();
}

struct FrameSample {
	double begin{0};
	double build{0};
	double record{0};
	double submit{0};
	double gpu{0};
	double total{0};
	uint32_t vertices{0};
	uint32_t draw_calls{0};
	uint32_t drawn{0};
	uint32_t culled{0};
	uint32_t occluded{0};
};

struct Stats {
	double median{0};
	double mean{0};
	double p10{0};
	double p90{0};
	double min{0};
	double max{0};
};

Stats summarize(std::vector<double> values)
{
	Stats stats;
	if (values.empty()) {
		return stats;
	}
	std::sort(values.begin(), values.end());
	const std::size_t n = values.size();
	const auto at = [&](double q) {
		return values[static_cast<std::size_t>(q * static_cast<double>(n - 1) + 0.5)];
	};
	stats.median = (n % 2 == 1) ? values[n / 2]
	                            : 0.5 * (values[n / 2 - 1] + values[n / 2]);
	double sum   = 0.0;
	for (const double v : values) {
		sum += v;
	}
	stats.mean = sum / static_cast<double>(n);
	stats.p10  = at(0.10);
	stats.p90  = at(0.90);
	stats.min  = values.front();
	stats.max  = values.back();
	return stats;
}

/// FNV-1a over the whole RGBA capture. Not cryptographic; the point is a
/// cheap "did a single byte of the final image change" signal.
uint64_t fnv1a(const std::vector<uint8_t>& bytes)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const uint8_t byte : bytes) {
		hash ^= byte;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

/// Box-filters RGBA down by \p divisor into tightly packed RGB. Edge texels
/// that do not fill a whole block are dropped (1280x720 divides evenly).
std::vector<uint8_t> thumbnail(const std::vector<uint8_t>& rgba, uint32_t width,
                               uint32_t height, uint32_t divisor,
                               uint32_t& out_w, uint32_t& out_h)
{
	out_w = width / divisor;
	out_h = height / divisor;
	std::vector<uint8_t> out(static_cast<std::size_t>(out_w) * out_h * 3u);
	const uint32_t area = divisor * divisor;
	for (uint32_t y = 0; y < out_h; ++y) {
		for (uint32_t x = 0; x < out_w; ++x) {
			uint32_t sum[3] = {0, 0, 0};
			for (uint32_t dy = 0; dy < divisor; ++dy) {
				for (uint32_t dx = 0; dx < divisor; ++dx) {
					const std::size_t at =
					        (static_cast<std::size_t>(y * divisor + dy) * width +
					         (x * divisor + dx)) *
					        4u;
					sum[0] += rgba[at];
					sum[1] += rgba[at + 1];
					sum[2] += rgba[at + 2];
				}
			}
			const std::size_t o =
			        (static_cast<std::size_t>(y) * out_w + x) * 3u;
			for (int c = 0; c < 3; ++c) {
				out[o + static_cast<std::size_t>(c)] =
				        static_cast<uint8_t>((sum[c] + area / 2u) / area);
			}
		}
	}
	return out;
}

std::string hash_path(const std::string& golden)
{
	return golden + ".hash";
}

std::string to_hex(uint64_t value)
{
	char buffer[17];
	std::snprintf(buffer, sizeof(buffer), "%016llx",
	              static_cast<unsigned long long>(value));
	return buffer;
}

/// The sidecar holds the full-resolution image hash and the device that
/// produced it. Bit-exactness is only meaningful on that same device and
/// driver (a different rasterizer legitimately differs by a few levels), so
/// the comparison reports it only when the device name matches.
bool write_golden(const std::string& path, const std::vector<uint8_t>& rgba,
                  uint32_t width, uint32_t height, const std::string& device)
{
	uint32_t tw = 0;
	uint32_t th = 0;
	const std::vector<uint8_t> thumb =
	        thumbnail(rgba, width, height, kThumbnailDivisor, tw, th);
	std::error_code ec;
	std::filesystem::create_directories(
	        std::filesystem::path(path).parent_path(), ec);
	if (!stbi_write_png(path.c_str(), static_cast<int>(tw),
	                    static_cast<int>(th), 3, thumb.data(),
	                    static_cast<int>(tw * 3u))) {
		std::cerr << "lotus_render_bench: cannot write " << path << "\n";
		return false;
	}
	std::ofstream out(hash_path(path));
	out << to_hex(fnv1a(rgba)) << "\n" << device << "\n";
	return static_cast<bool>(out);
}

struct Fidelity {
	bool compared{false};
	/// False when the golden came from a different device: \p exact is then
	/// meaningless and must not be reported.
	bool exact_applicable{false};
	bool exact{false};
	double mae{0.0};
	int max_diff{0};
};

/// Compares the final frame against the golden. Returns false on a hard error
/// (missing/unreadable golden, size mismatch): a benchmark that silently
/// skipped the fidelity check would let a broken renderer look fast.
bool compare_golden(const std::string& path, const std::vector<uint8_t>& rgba,
                    uint32_t width, uint32_t height, const std::string& device,
                    Fidelity& out)
{
	int gw = 0;
	int gh = 0;
	int gc = 0;
	uint8_t* golden = stbi_load(path.c_str(), &gw, &gh, &gc, 3);
	if (golden == nullptr) {
		std::cerr << "lotus_render_bench: cannot read golden " << path << "\n";
		return false;
	}
	uint32_t tw = 0;
	uint32_t th = 0;
	const std::vector<uint8_t> thumb =
	        thumbnail(rgba, width, height, kThumbnailDivisor, tw, th);
	if (static_cast<uint32_t>(gw) != tw || static_cast<uint32_t>(gh) != th) {
		std::cerr << "lotus_render_bench: golden is " << gw << "x" << gh
		          << " but the frame thumbnail is " << tw << "x" << th
		          << " (resolution changed? regenerate with --write-golden)\n";
		stbi_image_free(golden);
		return false;
	}
	double sum = 0.0;
	int worst  = 0;
	for (std::size_t i = 0; i < thumb.size(); ++i) {
		const int diff = std::abs(static_cast<int>(thumb[i]) -
		                          static_cast<int>(golden[i]));
		sum += diff;
		worst = std::max(worst, diff);
	}
	stbi_image_free(golden);
	out.compared = true;
	out.mae      = sum / static_cast<double>(thumb.size());
	out.max_diff = worst;

	std::ifstream in(hash_path(path));
	std::string stored_hash;
	std::string stored_device;
	in >> stored_hash;
	in.ignore(1, '\n');
	std::getline(in, stored_device);
	out.exact_applicable = !stored_hash.empty() && stored_device == device;
	out.exact = out.exact_applicable && stored_hash == to_hex(fnv1a(rgba));
	return true;
}

/// One synchronous frame: record, submit, wait for idle. Phase times and the
/// scene's workload counters are captured on the way.
FrameSample render_frame(GpuRenderer& gpu, Scene& scene, float aspect)
{
	const auto t0 = Clock::now();
	gpu.begin_frame_offscreen();
	const auto t1 = Clock::now();
	scene.upload(gpu, aspect);
	const auto t2 = Clock::now();
	gpu.begin_scene_pass();
	scene.draw(gpu);
	gpu.end_scene_pass();
	const auto t3 = Clock::now();
	gpu.end_frame();
	const auto t4 = Clock::now();
	gpu.wait_idle();
	const auto t5 = Clock::now();

	FrameSample sample;
	sample.begin      = elapsed_ms(t0, t1);
	sample.build      = elapsed_ms(t1, t2);
	sample.record     = elapsed_ms(t2, t3);
	sample.submit     = elapsed_ms(t3, t4);
	sample.gpu        = elapsed_ms(t4, t5);
	sample.total      = elapsed_ms(t0, t5);
	sample.vertices   = scene.vertex_count();
	sample.draw_calls = scene.draw_calls();
	sample.drawn      = scene.drawn_entities();
	sample.culled     = scene.culled_entities();
	sample.occluded   = scene.occluded_entities();
	return sample;
}

/// Pose of the untimed fidelity frames: a fixed point of the camera loop with
/// the movers frozen, so the captured image and the workload counters do not
/// depend on --frames, --rounds or --warmup.
constexpr float kFidelityPose = 0.25f;
/// Frames rendered at the fidelity pose before capturing. Occlusion culling
/// and SSAO read the previous frame: frame 1 has no history (everything is
/// drawn, no AO), frame 2 culls and shades from frame 1, frame 3 from frame 2.
/// At a static pose that is a fixed point, which is what gets captured.
constexpr int kFidelityFrames = 3;

/// Renders the fidelity frames, captures the result, and writes/compares the
/// golden. Runs first, on the fresh renderer: the frames before it are none,
/// so the Hi-Z and SSAO history that frame N+1 reads is a pure function of
/// the scene and pose, and the image does not depend on --frames, --rounds or
/// --warmup. Returns false on a hard error.
bool capture_fidelity(GpuRenderer& gpu, Scene& scene, World& world,
                      float aspect, const Options& options,
                      const std::string& device, FrameSample& counters,
                      Fidelity& fidelity, uint64_t& image_hash)
{
	animate(scene, world, kFidelityPose);
	for (int i = 0; i < kFidelityFrames; ++i) {
		counters = render_frame(gpu, scene, aspect);
	}
	std::printf("bench: fidelity frame vertices=%u draw_calls=%u drawn=%u "
	            "culled=%u occluded=%u\n",
	            counters.vertices, counters.draw_calls, counters.drawn,
	            counters.culled, counters.occluded);

	uint32_t width                  = 0;
	uint32_t height                 = 0;
	const std::vector<uint8_t> rgba = gpu.capture_rgba(width, height);
	if (rgba.empty()) {
		std::cerr << "lotus_render_bench: capture failed\n";
		return false;
	}
	image_hash = fnv1a(rgba);
	if (!options.ppm.empty() && !gpu.write_ppm(options.ppm)) {
		std::cerr << "lotus_render_bench: cannot write " << options.ppm << "\n";
		return false;
	}
	if (!options.write_golden.empty()) {
		if (!write_golden(options.write_golden, rgba, width, height, device)) {
			return false;
		}
		std::printf("bench: wrote golden %s (hash %s)\n",
		            options.write_golden.c_str(), to_hex(image_hash).c_str());
	}
	if (!options.golden.empty() &&
	    !compare_golden(options.golden, rgba, width, height, device,
	                    fidelity)) {
		return false;
	}
	std::printf("bench: image hash=%s\n", to_hex(image_hash).c_str());
	return true;
}

/// What a throughput run measured; the per-phase fields are sums.
struct Throughput {
	uint64_t frames{0};
	double seconds{0};
	double begin_ms{0};
	double build_ms{0};
	double record_ms{0};
	double submit_ms{0};
	double vertices{0};
	double drawn{0};
};

/// Frames go out back to back for options.seconds of wall time, waiting for
/// the GPU only through the renderer's own frames-in-flight limit, so the rate
/// is what a real loop reaches. begin_frame_offscreen is where that limit
/// blocks: its share grows when the GPU is the bottleneck and shrinks when the
/// CPU is.
Throughput run_throughput(GpuRenderer& gpu, Scene& scene, World& world,
                          float aspect, const Options& options)
{
	Throughput totals;
	int index        = 0;
	const auto frame = [&](bool timed) {
		animate(scene, world,
		        static_cast<float>(index % options.frames) /
		                static_cast<float>(options.frames));
		++index;
		const auto t0 = Clock::now();
		gpu.begin_frame_offscreen();
		const auto t1 = Clock::now();
		scene.upload(gpu, aspect);
		const auto t2 = Clock::now();
		gpu.begin_scene_pass();
		scene.draw(gpu);
		if (!options.occlusion) {
			gpu.suppress_occlusion_record();
		}
		gpu.end_scene_pass();
		const auto t3 = Clock::now();
		gpu.end_frame();
		const auto t4 = Clock::now();
		if (timed) {
			totals.begin_ms += elapsed_ms(t0, t1);
			totals.build_ms += elapsed_ms(t1, t2);
			totals.record_ms += elapsed_ms(t2, t3);
			totals.submit_ms += elapsed_ms(t3, t4);
			totals.vertices += scene.vertex_count();
			totals.drawn += scene.drawn_entities();
			++totals.frames;
		}
	};

	// Warm up the way the run will go (pipelined), so clocks, pipeline caches,
	// textures and the Hi-Z history have all settled before timing starts.
	for (int i = 0; i < options.warmup + 60; ++i) {
		frame(false);
	}
	gpu.wait_idle();

	const auto start = Clock::now();
	do {
		for (int i = 0; i < 32; ++i) {
			frame(true);
		}
	} while (elapsed_ms(start, Clock::now()) < options.seconds * 1000.0);
	gpu.wait_idle();
	totals.seconds = elapsed_ms(start, Clock::now()) / 1000.0;
	return totals;
}

void print_throughput(const Throughput& throughput)
{
	const double frames = static_cast<double>(throughput.frames);
	std::printf("bench: pipelined %llu frames in %.3f s\n",
	            static_cast<unsigned long long>(throughput.frames),
	            throughput.seconds);
	std::printf("METRIC fps=%.2f\n", frames / throughput.seconds);
	std::printf("METRIC mverts_per_s=%.2f\n",
	            throughput.vertices / throughput.seconds / 1.0e6);
	std::printf("METRIC pipe_frames=%llu\n",
	            static_cast<unsigned long long>(throughput.frames));
	std::printf("METRIC pipe_begin_ms=%.4f\n", throughput.begin_ms / frames);
	std::printf("METRIC pipe_build_ms=%.4f\n", throughput.build_ms / frames);
	std::printf("METRIC pipe_record_ms=%.4f\n", throughput.record_ms / frames);
	std::printf("METRIC pipe_submit_ms=%.4f\n", throughput.submit_ms / frames);
	std::printf("METRIC pipe_drawn_entities=%.1f\n", throughput.drawn / frames);
}

/// The device and the hash of the fidelity frame, which every mode reports so
/// a run can be matched to the image it rendered.
void print_identity(const std::string& device_name, uint64_t image_hash)
{
	std::string device_token = device_name;
	for (char& c : device_token) {
		if (!std::isalnum(static_cast<unsigned char>(c))) {
			c = '_';
		}
	}
	std::printf("ASI device=%s\n", device_token.c_str());
	std::printf("ASI image_hash=%s\n", to_hex(image_hash).c_str());
}

int run_gpu(const Options& options, Scene& scene, World& world)
{
	GpuRenderer gpu;
	gpu.init_offscreen(options.width, options.height);
	configure_lighting(gpu);

	const char* device_name = gpu.device_name();
	const char* driver_name = gpu.device_driver();
	const float aspect = static_cast<float>(options.width) /
	                     static_cast<float>(options.height);
	std::printf("bench: device=\"%s\" driver=\"%s\" size=%ux%u "
	            "entities=%zu movers=%zu warmup=%d frames=%dx%d seed=%llu\n",
	            device_name, driver_name, options.width, options.height,
	            world.entities, world.movers.size(), options.warmup,
	            options.frames, options.rounds,
	            static_cast<unsigned long long>(options.seed));

	FrameSample counters;
	Fidelity fidelity;
	uint64_t image_hash = 0;
	if (!capture_fidelity(gpu, scene, world, aspect, options, device_name,
	                      counters, fidelity, image_hash)) {
		return 1;
	}

	// Per-frame hashes: the standard camera loop, synchronous, every frame
	// captured and hashed, so two builds can be diffed frame by frame.
	if (options.frame_hashes > 0) {
		for (int frame = 0; frame < options.frame_hashes; ++frame) {
			animate(scene, world,
			        static_cast<float>(frame % options.frames) /
			                static_cast<float>(options.frames));
			render_frame(gpu, scene, aspect);
			uint32_t width  = 0;
			uint32_t height = 0;
			const std::vector<uint8_t> pixels = gpu.capture_rgba(width, height);
			std::printf("FRAMEHASH %d %s\n", frame,
			            to_hex(fnv1a(pixels)).c_str());
		}
		print_identity(device_name, image_hash);
		return 0;
	}

	if (options.pipelined) {
		print_throughput(run_throughput(gpu, scene, world, aspect, options));
		print_identity(device_name, image_hash);
		return 0;
	}

	std::vector<FrameSample> samples;
	samples.reserve(static_cast<std::size_t>(options.frames) *
	                static_cast<std::size_t>(options.rounds));
	const int total_frames = options.warmup + options.frames * options.rounds;
	for (int frame = 0; frame < total_frames; ++frame) {
		// Each round replays the same loop of the camera path, so rounds are
		// directly comparable. Warmup is the tail of the loop that precedes
		// the first round, which keeps the camera continuous (a jump would
		// make the first frame cull against a stale Hi-Z).
		const int timed_index = frame - options.warmup;
		const int slot = ((timed_index % options.frames) + options.frames) %
		                 options.frames;
		animate(scene, world,
		        static_cast<float>(slot) / static_cast<float>(options.frames));
		const FrameSample sample = render_frame(gpu, scene, aspect);
		if (timed_index >= 0) {
			samples.push_back(sample);
		}
	}

	const auto column = [&](double FrameSample::*member) {
		std::vector<double> values;
		values.reserve(samples.size());
		for (const FrameSample& sample : samples) {
			values.push_back(sample.*member);
		}
		return summarize(std::move(values));
	};
	const Stats begin  = column(&FrameSample::begin);
	const Stats build  = column(&FrameSample::build);
	const Stats record = column(&FrameSample::record);
	const Stats submit = column(&FrameSample::submit);
	const Stats gpu_ms = column(&FrameSample::gpu);
	const Stats total  = column(&FrameSample::total);
	// CPU-side cost per frame: everything before the GPU wait. Summed per
	// frame (not as a sum of medians) so it is a real distribution.
	std::vector<double> cpu_values;
	cpu_values.reserve(samples.size());
	for (const FrameSample& sample : samples) {
		cpu_values.push_back(sample.total - sample.gpu);
	}
	const Stats cpu = summarize(std::move(cpu_values));

	std::printf("bench: %-8s %9s %9s %9s %9s %9s %9s\n", "phase(ms)", "median",
	            "mean", "p10", "p90", "min", "max");
	const auto row = [](const char* name, const Stats& s) {
		std::printf("bench: %-8s %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f\n", name,
		            s.median, s.mean, s.p10, s.p90, s.min, s.max);
	};
	row("begin", begin);
	row("build", build);
	row("record", record);
	row("submit", submit);
	row("gpu", gpu_ms);
	row("cpu", cpu);
	row("frame", total);

	// Median per round: a drifting machine (thermals, a noisy neighbour)
	// shows up as rounds that disagree, which the pooled median hides.
	std::printf("bench: frame median per round (ms):");
	for (int round = 0; round < options.rounds; ++round) {
		std::vector<double> values;
		for (int i = 0; i < options.frames; ++i) {
			values.push_back(
			        samples[static_cast<std::size_t>(round * options.frames + i)]
			                .total);
		}
		std::printf(" %.3f", summarize(std::move(values)).median);
	}
	std::printf("\n");

	std::printf("METRIC frame_ms=%.4f\n", total.median);
	std::printf("METRIC cpu_ms=%.4f\n", cpu.median);
	std::printf("METRIC gpu_ms=%.4f\n", gpu_ms.median);
	std::printf("METRIC begin_ms=%.4f\n", begin.median);
	std::printf("METRIC build_ms=%.4f\n", build.median);
	std::printf("METRIC record_ms=%.4f\n", record.median);
	std::printf("METRIC submit_ms=%.4f\n", submit.median);
	std::printf("METRIC frame_ms_p90=%.4f\n", total.p90);
	std::printf("METRIC vertices=%u\n", counters.vertices);
	std::printf("METRIC draw_calls=%u\n", counters.draw_calls);
	std::printf("METRIC drawn_entities=%u\n", counters.drawn);
	if (fidelity.compared) {
		std::printf("METRIC image_mae=%.5f\n", fidelity.mae);
		std::printf("METRIC image_max_diff=%d\n", fidelity.max_diff);
		if (fidelity.exact_applicable) {
			std::printf("METRIC image_exact=%d\n", fidelity.exact ? 1 : 0);
		}
	}
	print_identity(device_name, image_hash);
	return 0;
}

int run(const Options& options)
{
	AssetRegistry assets;
	if (!write_textures(options.assets_dir, assets)) {
		return 1;
	}
	Scene scene{"render_bench"};
	scene.set_assets(&assets, options.assets_dir);
	World world = build_world(scene, options);
	animate(scene, world, 0.0f);
	scene.update(0.0f);

	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::cerr << "lotus_render_bench: SDL video init failed: "
		          << SDL_GetError() << "\n";
		return 1;
	}
	int status = 1;
	try {
		status = run_gpu(options, scene, world); // device dies before SDL_Quit
	} catch (...) {
		SDL_Quit();
		throw;
	}
	SDL_Quit();
	return status;
}

} // namespace

int main(int argc, char** argv)
{
	Options options;
	if (!parse_options(argc, argv, options)) {
		std::cerr << kUsage << "\n";
		return 2;
	}
	try {
		return run(options);
	} catch (const std::exception& ex) {
		std::cerr << "lotus_render_bench: " << ex.what() << "\n";
		return 1;
	}
}
