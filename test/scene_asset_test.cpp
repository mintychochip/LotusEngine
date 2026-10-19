#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "lotus/scene/assets.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;
namespace fs = std::filesystem;

namespace {

const unsigned char k1x1Png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
        0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
        0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB4, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

fs::path write_textured_glb(const fs::path& path)
{
	const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
	const float normals[9]   = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
	const std::uint16_t indices[3] = {0, 1, 2};
	std::vector<std::uint8_t> bin;
	const auto append = [&bin](const void* data, std::size_t bytes) {
		const auto* first = static_cast<const std::uint8_t*>(data);
		bin.insert(bin.end(), first, first + bytes);
	};
	append(positions, sizeof(positions));
	append(normals, sizeof(normals));
	append(indices, sizeof(indices));
	while (bin.size() % 4 != 0) {
		bin.push_back(0);
	}
	const std::size_t image_offset = bin.size();
	append(k1x1Png, sizeof(k1x1Png));
	const std::string json = R"({
      "asset":{"version":"2.0"},
      "buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":36},
        {"buffer":0,"byteOffset":36,"byteLength":36},
        {"buffer":0,"byteOffset":72,"byteLength":6},
        {"buffer":0,"byteOffset":)" + std::to_string(image_offset) + R"(,"byteLength":)" +
	                         std::to_string(sizeof(k1x1Png)) + R"(}
      ],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}
      ],
      "images":[{"bufferView":3,"mimeType":"image/png"}],
      "textures":[{"source":0}],
      "materials":[{"pbrMetallicRoughness":{
        "baseColorFactor":[0.1,0.2,0.3,1],
        "baseColorTexture":{"index":0}
      }}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2,"material":0}]}]
    })";
	std::vector<std::uint8_t> json_bytes(json.begin(), json.end());
	while (json_bytes.size() % 4 != 0) {
		json_bytes.push_back(' ');
	}
	std::vector<std::uint8_t> glb;
	const auto append32 = [&glb](std::uint32_t value) {
		glb.push_back(static_cast<std::uint8_t>(value & 0xFF));
		glb.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
		glb.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
		glb.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
	};
	const std::uint32_t total = 12 + 8 +
	                            static_cast<std::uint32_t>(json_bytes.size()) + 8 +
	                            static_cast<std::uint32_t>(bin.size());
	append32(0x46546C67);
	append32(2);
	append32(total);
	append32(static_cast<std::uint32_t>(json_bytes.size()));
	append32(0x4E4F534A);
	glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
	append32(static_cast<std::uint32_t>(bin.size()));
	append32(0x004E4942);
	glb.insert(glb.end(), bin.begin(), bin.end());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(glb.data()),
	          static_cast<std::streamsize>(glb.size()));
	return path;
}

} // namespace

TEST(SceneAssetTest, MeshResolvesThroughRegistryKey)
{
	AssetRegistry registry;
	registry.set("hero_mesh", "builtin:cube");

	Scene scene{"test"};
	scene.set_assets(&registry);
	const Entity entity = scene.create("mesh");
	scene.registry().emplace<MeshSource>(
	        entity, MeshSource{"builtin:sphere", "hero_mesh"});

	scene.update(0.0f);

	ASSERT_TRUE(scene.registry().all_of<Mesh>(entity));
	const Mesh& mesh = scene.registry().get<Mesh>(entity);
	EXPECT_EQ(mesh.source, "builtin:cube");
	EXPECT_EQ(mesh.indices.size(), 36u);
	EXPECT_TRUE(scene.last_mesh_error().empty());
}

TEST(SceneAssetTest, StalePathStillLoadsWhenKeyKnown)
{
	AssetRegistry registry;
	registry.set("cube_key", "builtin:cube");

	Scene scene{"test"};
	scene.set_assets(&registry);
	const Entity entity = scene.create("mesh");
	scene.registry().emplace<MeshSource>(
	        entity, MeshSource{"assets/moved-away.glb", "cube_key"});

	scene.update(0.0f);

	ASSERT_TRUE(scene.registry().all_of<Mesh>(entity));
	EXPECT_EQ(scene.registry().get<Mesh>(entity).source, "builtin:cube");
}

TEST(SceneAssetTest, GlbSyncAppliesMaterialAlbedoKey)
{
	const fs::path root = fs::temp_directory_path() / "lotus_scene_glb_mat";
	const fs::path assets = root / "assets";
	fs::create_directories(assets);
	const fs::path glb_path = write_textured_glb(root / "prop.glb");

	AssetRegistry registry;
	Scene scene{"test"};
	scene.set_assets(&registry, assets.string());
	const Entity entity = scene.create("prop");
	scene.registry().emplace<MeshSource>(entity, MeshSource{glb_path.string(), ""});

	scene.update(0.0f);

	ASSERT_TRUE(scene.registry().all_of<Material>(entity));
	EXPECT_EQ(scene.registry().get<Material>(entity).albedo_id, "prop#0");
	EXPECT_TRUE(registry.find("prop#0") != nullptr);

	fs::remove_all(root);
}
