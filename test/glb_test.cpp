#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/assets.hpp"
#include "lotus/scene/glb.hpp"
#include "lotus/scene/mesh.hpp"

using namespace lotus;
namespace fs = std::filesystem;

namespace {

/// Builds a minimal but valid GLB holding one triangle (position + normal +
/// u16 indices) so the loader is tested without a committed asset.
std::vector<std::uint8_t> build_triangle_glb()
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

	const std::string json = R"({
      "asset":{"version":"2.0"},
      "buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":36},
        {"buffer":0,"byteOffset":36,"byteLength":36},
        {"buffer":0,"byteOffset":72,"byteLength":6}
      ],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}
      ],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}]
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
	append32(0x46546C67); // "glTF"
	append32(2);
	append32(total);
	append32(static_cast<std::uint32_t>(json_bytes.size()));
	append32(0x4E4F534A); // "JSON"
	glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
	append32(static_cast<std::uint32_t>(bin.size()));
	append32(0x004E4942); // "BIN\0"
	glb.insert(glb.end(), bin.begin(), bin.end());
	return glb;
}

/// 1x1 RGB PNG (red pixel).
const unsigned char k1x1Png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
        0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
        0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB4, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};
constexpr std::size_t k1x1PngSize = sizeof(k1x1Png);

std::vector<std::uint8_t> build_textured_triangle_glb()
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
	append(k1x1Png, k1x1PngSize);
	while (bin.size() % 4 != 0) {
		bin.push_back(0);
	}

	const std::string json = R"({
      "asset":{"version":"2.0"},
      "buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":36},
        {"buffer":0,"byteOffset":36,"byteLength":36},
        {"buffer":0,"byteOffset":72,"byteLength":6},
        {"buffer":0,"byteOffset":)" + std::to_string(image_offset) + R"(,"byteLength":)" +
	                         std::to_string(k1x1PngSize) + R"(}
      ],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}
      ],
      "images":[{"bufferView":3,"mimeType":"image/png"}],
      "textures":[{"source":0}],
      "materials":[{"pbrMetallicRoughness":{
        "baseColorFactor":[0.2,0.4,0.9,1],
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
	return glb;
}

fs::path write_temp_glb(const std::string& name)
{
	const std::vector<std::uint8_t> glb = build_triangle_glb();
	const fs::path path = fs::temp_directory_path() / name;
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(glb.data()),
	          static_cast<std::streamsize>(glb.size()));
	return path;
}

} // namespace

TEST(GlbTest, LoadsPositionsNormalsAndIndices)
{
	const fs::path path = write_temp_glb("lotus_triangle.glb");

	Mesh mesh;
	std::string error;
	ASSERT_TRUE(load_glb(path.string(), mesh, &error)) << error;

	ASSERT_EQ(mesh.positions.size(), 3u);
	ASSERT_EQ(mesh.normals.size(), 3u);
	ASSERT_EQ(mesh.indices.size(), 3u);
	EXPECT_EQ(mesh.triangle_count(), 1u);
	EXPECT_EQ(mesh.indices[0], 0u);
	EXPECT_EQ(mesh.indices[1], 1u);
	EXPECT_EQ(mesh.indices[2], 2u);
	EXPECT_FLOAT_EQ(mesh.positions[1].x, 1.0f);
	EXPECT_FLOAT_EQ(mesh.normals[0].z, 1.0f);

	fs::remove(path);
}

TEST(GlbTest, ResolvesThroughMeshSource)
{
	const fs::path path = write_temp_glb("lotus_source.glb");

	Mesh mesh;
	std::string error;
	ASSERT_TRUE(mesh::resolve_mesh(path.string(), mesh, &error)) << error;
	EXPECT_EQ(mesh.positions.size(), 3u);

	fs::remove(path);
}

TEST(GlbTest, ImportsEmbeddedBaseColorTexture)
{
	const fs::path root = fs::temp_directory_path() / "lotus_glb_tex";
	const fs::path assets = root / "assets";
	fs::create_directories(assets);
	const fs::path glb_path = root / "hero.glb";
	{
		const std::vector<std::uint8_t> glb = build_textured_triangle_glb();
		std::ofstream out(glb_path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(glb.data()),
		          static_cast<std::streamsize>(glb.size()));
	}

	AssetRegistry registry;
	std::vector<GlbMaterialSlot> slots;
	int first_material = -1;
	std::string error;
	ASSERT_TRUE(import_glb_materials(glb_path.string(), "hero", assets.string(),
	                                 registry, slots, first_material, &error))
	        << error;
	EXPECT_EQ(first_material, 0);
	EXPECT_EQ(slots.size(), 1u);
	EXPECT_EQ(slots[0].albedo_id, "hero#0");
	EXPECT_NEAR(slots[0].base_color.b, 0.9f, 1e-3f);
	const std::string* path = registry.find("hero#0");
	ASSERT_NE(path, nullptr);
	EXPECT_TRUE(fs::exists(*path));

	fs::remove_all(root);
}

TEST(GlbTest, RejectsNonGlbFiles)
{
	const fs::path path = fs::temp_directory_path() / "lotus_not_a_glb.bin";
	{
		std::ofstream out(path, std::ios::binary);
		out << "definitely not a glb";
	}

	Mesh mesh;
	std::string error;
	EXPECT_FALSE(load_glb(path.string(), mesh, &error));
	EXPECT_FALSE(error.empty());

	fs::remove(path);
}

TEST(GlbTest, RejectsMissingFile)
{
	Mesh mesh;
	std::string error;
	EXPECT_FALSE(load_glb("definitely_missing_model.glb", mesh, &error));
	EXPECT_FALSE(error.empty());
}

TEST(GlbTest, RejectsTruncatedGlb)
{
	const std::vector<std::uint8_t> full = build_triangle_glb();
	std::vector<std::uint8_t> truncated(full.begin(), full.begin() + 20);
	const fs::path path = fs::temp_directory_path() / "lotus_truncated.glb";
	{
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(truncated.data()),
		          static_cast<std::streamsize>(truncated.size()));
	}

	Mesh mesh;
	std::string error;
	EXPECT_FALSE(load_glb(path.string(), mesh, &error));

	fs::remove(path);
}
