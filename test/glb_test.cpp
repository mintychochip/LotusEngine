#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

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
