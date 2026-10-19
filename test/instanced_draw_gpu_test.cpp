#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <SDL3/SDL.h>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/mesh.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

namespace {

/// A headless Vulkan device still needs SDL's video subsystem; the offscreen
/// video driver needs no display, which is what lets this run in a container.
class InstancedDrawGpuTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
		owned_sdl_ = SDL_Init(SDL_INIT_VIDEO);
		ASSERT_TRUE(owned_sdl_) << SDL_GetError();
	}

	void TearDown() override
	{
		if (owned_sdl_) {
			SDL_Quit();
		}
	}

	bool owned_sdl_{false};
};

constexpr uint32_t kSize = 256;

struct Shape {
	const char* source;
	glm::vec3 position;
	glm::vec3 rotation;
	glm::vec3 scale;
	glm::vec3 color;
	float roughness;
};

/// Three different meshes with rotation, non-uniform scale, distinct colours
/// and roughness: a wrong model matrix, a swapped normal matrix or a lost
/// material multiplier each moves a lot of pixels.
const std::array<Shape, 3> kShapes = {{
        {"builtin:cube",
         {-1.5f, 0.0f, 0.0f},
         {0.0f, 0.6f, 0.0f},
         {1.0f, 1.4f, 0.8f},
         {0.78f, 0.32f, 0.22f},
         0.55f},
        {"builtin:sphere",
         {1.4f, 0.3f, 0.5f},
         {0.0f, 0.0f, 0.0f},
         {1.2f, 1.2f, 1.2f},
         {0.22f, 0.47f, 0.55f},
         0.25f},
        {"builtin:cylinder",
         {0.0f, -0.9f, -1.0f},
         {0.5f, 0.0f, 0.2f},
         {1.0f, 1.0f, 1.0f},
         {0.86f, 0.71f, 0.5f},
         0.75f},
}};

void add_camera(Scene& scene)
{
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera,
	                                   Camera3D{{0.0f, 1.0f, 7.0f},
	                                            {0.0f, 0.0f, 0.0f},
	                                            {0.0f, 1.0f, 0.0f},
	                                            50.0f,
	                                            0.1f,
	                                            100.0f});
}

/// The shapes above. \p instanced says, per shape, whether it is a mesh with a
/// source (cached on the GPU, drawn instanced) or the same geometry built by
/// hand with no source (written as world-space vertices every frame).
Scene make_scene(const std::array<bool, 3>& instanced)
{
	Scene scene{"s"};
	add_camera(scene);
	for (std::size_t i = 0; i < kShapes.size(); ++i) {
		const Shape& shape  = kShapes[i];
		const Entity entity = scene.create(shape.source);
		if (instanced[i]) {
			scene.registry().emplace<MeshSource>(
			        entity, MeshSource{shape.source, ""});
		} else {
			Mesh hand_built;
			EXPECT_TRUE(mesh::resolve_mesh(shape.source, hand_built));
			hand_built.bounds = mesh::compute_mesh_bounds(hand_built);
			scene.registry().emplace<Mesh>(entity, std::move(hand_built));
		}
		Transform& transform = scene.registry().get<Transform>(entity);
		transform.position   = shape.position;
		transform.rotation   = shape.rotation;
		transform.scale      = shape.scale;
		scene.registry().emplace<Material>(
		        entity, Material{shape.color, "", shape.roughness});
	}
	scene.update(0.0f);
	return scene;
}

/// Renders \p frames frames, each run to completion so that the occlusion and
/// SSAO history the next frame reads is the same on every run, then returns
/// the last one as RGBA.
std::vector<uint8_t> render(GpuRenderer& gpu, Scene& scene, int frames)
{
	for (int frame = 0; frame < frames; ++frame) {
		gpu.begin_frame_offscreen();
		scene.upload(gpu, 1.0f);
		gpu.begin_scene_pass();
		scene.draw(gpu);
		gpu.end_scene_pass();
		gpu.end_frame();
		gpu.wait_idle();
	}
	uint32_t width  = 0;
	uint32_t height = 0;
	return gpu.capture_rgba(width, height);
}

struct Difference {
	double mean_abs{0.0};
	std::size_t large{0}; // pixels whose worst channel differs by more than 16
	std::size_t pixels{0};
};

Difference compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	Difference out;
	out.pixels = a.size() / 4;
	double sum = 0.0;
	for (std::size_t pixel = 0; pixel < out.pixels; ++pixel) {
		int worst = 0;
		for (std::size_t channel = 0; channel < 3; ++channel) {
			const int diff = std::abs(static_cast<int>(a[pixel * 4 + channel]) -
			                          static_cast<int>(b[pixel * 4 + channel]));
			sum += diff;
			worst = std::max(worst, diff);
		}
		out.large += worst > 16 ? 1u : 0u;
	}
	out.mean_abs = sum / (3.0 * static_cast<double>(out.pixels));
	return out;
}

/// What a different rasterised position can and cannot explain. The two paths
/// place vertices with the same maths but not the same rounding (the GPU
/// transforms instanced vertices, the CPU the others), which flips the
/// coverage of the odd pixel on a silhouette edge. A wrong matrix or a lost
/// material moves orders of magnitude more than this allows.
void expect_same_picture(const Difference& diff)
{
	EXPECT_LT(diff.mean_abs, 0.05);
	EXPECT_LE(diff.large, diff.pixels / 500)
	        << diff.large << " of " << diff.pixels
	        << " pixels differ by more than 16 levels";
}

} // namespace

/// The instance path must put every vertex where the world-space path puts it,
/// with the same normal and the same material: all instanced, none instanced,
/// and a frame that mixes both (which draws the world-space stream with the
/// reserved identity instance, next to real instances).
TEST_F(InstancedDrawGpuTest, InstancedMeshesRenderLikeTheirWorldSpaceVertices)
{
	const auto picture = [](const std::array<bool, 3>& instanced,
	                        uint32_t expected_cached, uint32_t& draw_calls,
	                        uint32_t& vertices) {
		GpuRenderer gpu;
		EXPECT_NO_THROW(gpu.init_offscreen(kSize, kSize));
		Scene scene = make_scene(instanced);
		const std::vector<uint8_t> image = render(gpu, scene, 3);
		EXPECT_EQ(gpu.cached_mesh_count(), expected_cached);
		EXPECT_EQ(scene.drawn_entities(), 3u);
		draw_calls = scene.draw_calls();
		vertices   = scene.vertex_count();
		return image;
	};

	uint32_t draw_calls_all = 0, vertices_all = 0;
	uint32_t draw_calls_none = 0, vertices_none = 0;
	uint32_t draw_calls_mixed = 0, vertices_mixed = 0;
	const std::vector<uint8_t> all =
	        picture({true, true, true}, 3, draw_calls_all, vertices_all);
	const std::vector<uint8_t> none =
	        picture({false, false, false}, 0, draw_calls_none, vertices_none);
	const std::vector<uint8_t> mixed =
	        picture({true, false, true}, 2, draw_calls_mixed, vertices_mixed);

	// Same triangles submitted whichever way they are drawn.
	EXPECT_EQ(vertices_all, vertices_none);
	EXPECT_EQ(vertices_all, vertices_mixed);
	// One draw per mesh when instanced; one range of the stream otherwise
	// (all three share the default albedo); a mix is both kinds.
	EXPECT_EQ(draw_calls_all, 3u);
	EXPECT_EQ(draw_calls_none, 1u);
	EXPECT_EQ(draw_calls_mixed, 3u);

	expect_same_picture(compare(all, none));
	expect_same_picture(compare(all, mixed));

	// The comparison is only meaningful if something was drawn: the same
	// frame with the shapes removed looks very different.
	GpuRenderer gpu;
	ASSERT_NO_THROW(gpu.init_offscreen(kSize, kSize));
	Scene empty{"empty"};
	add_camera(empty);
	const Difference against_empty = compare(all, render(gpu, empty, 3));
	EXPECT_GT(against_empty.large, against_empty.pixels / 50)
	        << "the shapes covered under 2% of the frame";
}

/// Entities of one mesh are one cached mesh and one draw, however many there
/// are, and a second albedo splits only the draw, never the cache.
TEST_F(InstancedDrawGpuTest, EntitiesOfOneMeshShareOneCachedMeshAndOneDraw)
{
	GpuRenderer gpu;
	ASSERT_NO_THROW(gpu.init_offscreen(kSize, kSize));

	Scene scene{"s"};
	add_camera(scene);
	for (int i = 0; i < 5; ++i) {
		const Entity cube = scene.create("cube");
		scene.registry().emplace<MeshSource>(cube,
		                                     MeshSource{"builtin:cube", ""});
		scene.registry().get<Transform>(cube).position = {
		        -2.0f + static_cast<float>(i), 0.0f, 0.0f};
		scene.registry().get<Transform>(cube).scale = {0.6f, 0.6f, 0.6f};
		scene.registry().emplace<Material>(cube, Material{});
	}
	scene.update(0.0f);

	(void)render(gpu, scene, 1);
	const uint32_t cube_indices = static_cast<uint32_t>(mesh::cube().indices.size());
	EXPECT_EQ(scene.drawn_entities(), 5u);
	EXPECT_EQ(gpu.cached_mesh_count(), 1u);
	EXPECT_EQ(scene.draw_calls(), 1u);
	EXPECT_EQ(scene.vertex_count(), 5u * cube_indices);

	// One of them moves to another albedo: still one mesh, now two draws.
	scene.registry().get<Material>(scene.find("cube")).albedo_id = "other";
	(void)render(gpu, scene, 1);
	EXPECT_EQ(gpu.cached_mesh_count(), 1u);
	EXPECT_EQ(scene.draw_calls(), 2u);
	EXPECT_EQ(scene.vertex_count(), 5u * cube_indices);
}

/// A mesh nobody draws any more must not stay on the GPU for ever (a streaming
/// world loads and unloads regions for hours), and drawing it again brings it
/// back looking the same.
TEST_F(InstancedDrawGpuTest, MeshesThatStopBeingDrawnAreReleasedAndComeBack)
{
	GpuRenderer gpu;
	ASSERT_NO_THROW(gpu.init_offscreen(64, 64));

	Scene with_cube = make_scene({true, false, false});
	Scene nothing{"nothing"};
	add_camera(nothing);

	const std::vector<uint8_t> before = render(gpu, with_cube, 3);
	ASSERT_EQ(gpu.cached_mesh_count(), 1u);

	// A short absence does not cost the upload.
	(void)render(gpu, nothing, 10);
	EXPECT_EQ(gpu.cached_mesh_count(), 1u);

	// A long one does. Eviction is checked every 64th frame, so allow for it.
	(void)render(gpu, nothing, GpuRenderer::kMeshIdleFrames + 80);
	EXPECT_EQ(gpu.cached_mesh_count(), 0u);

	const std::vector<uint8_t> after = render(gpu, with_cube, 3);
	EXPECT_EQ(gpu.cached_mesh_count(), 1u);
	EXPECT_EQ(compare(before, after).large, 0u)
	        << "the re-uploaded mesh no longer draws the same";
}

/// A source names one geometry. An entity that reuses a source for geometry of
/// a different size is drawn as the world-space vertices it really has, not as
/// an instance of someone else's mesh.
TEST_F(InstancedDrawGpuTest, ASourceReusedForOtherGeometryIsNotDrawnAsAnInstance)
{
	GpuRenderer gpu;
	ASSERT_NO_THROW(gpu.init_offscreen(kSize, kSize));

	Scene scene{"s"};
	add_camera(scene);
	const Entity honest = scene.create("honest");
	scene.registry().emplace<MeshSource>(honest,
	                                     MeshSource{"builtin:cube", ""});
	scene.registry().emplace<Material>(honest, Material{});

	// A sphere that claims to be the cube.
	Mesh impostor;
	ASSERT_TRUE(mesh::resolve_mesh("builtin:sphere", impostor));
	impostor.bounds = mesh::compute_mesh_bounds(impostor);
	impostor.source = "builtin:cube";
	const std::size_t sphere_indices = impostor.indices.size();
	const Entity liar                = scene.create("liar");
	scene.registry().emplace<Mesh>(liar, std::move(impostor));
	scene.registry().get<Transform>(liar).position = {2.0f, 0.0f, 0.0f};
	scene.registry().emplace<Material>(liar, Material{});
	scene.update(0.0f);

	(void)render(gpu, scene, 1);
	EXPECT_EQ(scene.drawn_entities(), 2u);
	// The cube is the one cached mesh; the sphere is the stream.
	EXPECT_EQ(gpu.cached_mesh_count(), 1u);
	EXPECT_EQ(scene.draw_calls(), 2u);
	EXPECT_EQ(scene.vertex_count(),
	          mesh::cube().indices.size() + sphere_indices);
}
