#include <gtest/gtest.h>

#include <cmath>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

namespace {

// Hemispherical integral of (diffuse + specular) * NdotL for unit white
// albedo with the light at normal incidence and the view at view_theta.
// A physical BRDF never reflects more than it receives, so this stays <= 1;
// the 1.05 bound leaves room for quadrature error.
float total_reflectance(float view_theta, float roughness)
{
	const float cv = std::cos(view_theta);
	const float sv = std::sin(view_theta);
	constexpr int kTheta = 64;
	constexpr int kPhi   = 128;
	const float d_theta  = (pbr::kPi / 2.0f) / kTheta;
	const float d_phi    = (2.0f * pbr::kPi) / kPhi;
	double sum           = 0.0;
	for (int i = 0; i < kTheta; ++i) {
		const float theta = (i + 0.5f) * d_theta;
		const float ct    = std::cos(theta);
		const float st    = std::sin(theta);
		for (int j = 0; j < kPhi; ++j) {
			const float phi = (j + 0.5f) * d_phi;
			// Outgoing direction; light is +Z so h = normalize(l + o).
			const float ox = st * std::cos(phi);
			const float oy = st * std::sin(phi);
			float hx = ox, hy = oy, hz = 1.0f + ct;
			const float hl = std::sqrt(hx * hx + hy * hy + hz * hz);
			hx /= hl;
			hy /= hl;
			hz /= hl;
			const float n_dot_h = hz;
			const float h_dot_v =
			        std::max(hx * sv + hz * cv, 0.0f);
			const float f       = pbr::fresnel_schlick(h_dot_v);
			const float diffuse = (1.0f - f) / pbr::kPi;
			const float spec    = pbr::specular(1.0f, cv, n_dot_h,
			                                    h_dot_v, roughness);
			sum += (diffuse + spec) * ct * st * d_theta * d_phi;
		}
	}
	return static_cast<float>(sum);
}

} // namespace

TEST(MaterialBatchTest, DistinctAlbedoIdsProduceSeparateDrawCalls)
{
	Scene scene{"test"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	scene.registry().emplace<MeshSource>(a, MeshSource{"builtin:cube", ""});
	scene.registry().emplace<MeshSource>(b, MeshSource{"builtin:cube", ""});
	scene.registry().emplace<Material>(a, Material{{1.0f, 1.0f, 1.0f}, "tex_a"});
	scene.registry().emplace<Material>(b, Material{{1.0f, 1.0f, 1.0f}, "tex_b"});

	scene.update(0.0f);
	(void)scene.build_vertices(1.0f);

	EXPECT_EQ(scene.draw_calls(), 2u);
}

TEST(MaterialBatchTest, SameAlbedoIdMergesIntoOneDrawCall)
{
	Scene scene{"test"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	scene.registry().emplace<MeshSource>(a, MeshSource{"builtin:cube", ""});
	scene.registry().emplace<MeshSource>(b, MeshSource{"builtin:sphere", ""});
	scene.registry().emplace<Material>(a, Material{{1.0f, 1.0f, 1.0f}, "shared"});
	scene.registry().emplace<Material>(b, Material{{1.0f, 1.0f, 1.0f}, "shared"});

	scene.update(0.0f);
	(void)scene.build_vertices(1.0f);

	EXPECT_EQ(scene.draw_calls(), 1u);
}

TEST(PbrBrdfTest, HemisphericalReflectanceNeverExceedsOne)
{
	// Energy conservation: dielectric Cook-Torrance must reflect <= what
	// arrives, for several roughnesses and view angles.
	for (const float roughness : {0.4f, 0.6f, 0.9f}) {
		for (const float view : {0.0f, pbr::kPi / 6.0f, pbr::kPi / 3.0f}) {
			const float total = total_reflectance(view, roughness);
			EXPECT_LE(total, 1.05f)
			        << "roughness=" << roughness << " view=" << view;
			EXPECT_GT(total, 0.5f)
			        << "roughness=" << roughness << " view=" << view;
		}
	}
}

TEST(PbrBrdfTest, GgxDistributionIntegratesToOne)
{
	// Normalization: the GGX NDF integrates to 1 over the hemisphere.
	constexpr int kTheta = 256;
	constexpr int kPhi   = 64;
	const float d_theta  = (pbr::kPi / 2.0f) / kTheta;
	const float d_phi    = (2.0f * pbr::kPi) / kPhi;
	double sum           = 0.0;
	for (int i = 0; i < kTheta; ++i) {
		const float theta = (i + 0.5f) * d_theta;
		const float ct    = std::cos(theta);
		sum += pbr::ndf_ggx(ct, 0.5f) * ct * std::sin(theta) * d_theta *
		       d_phi * kPhi;
	}
	EXPECT_NEAR(sum, 1.0, 0.05);
}

TEST(PbrBrdfTest, SpecularPeakFallsAsRoughnessRises)
{
	// Monotonic roughness response at perfect alignment: a mirror-like
	// surface concentrates the lobe, a rough one spreads it.
	float previous = pbr::specular(1.0f, 1.0f, 1.0f, 1.0f, 0.0f);
	EXPECT_GT(previous, 0.0f);
	for (int step = 1; step <= 20; ++step) {
		const float roughness = step / 20.0f;
		const float current =
		        pbr::specular(1.0f, 1.0f, 1.0f, 1.0f, roughness);
		EXPECT_LT(current, previous) << "roughness=" << roughness;
		previous = current;
	}
}

TEST(PbrBrdfTest, FresnelSchlickRunsFromF0ToOne)
{
	// Normal incidence reflects F0 (4% for dielectrics); grazing incidence
	// reflects everything; the curve is monotonic between them.
	EXPECT_FLOAT_EQ(pbr::fresnel_schlick(1.0f), pbr::kDielectricF0);
	EXPECT_FLOAT_EQ(pbr::fresnel_schlick(0.0f), 1.0f);
	EXPECT_GT(pbr::fresnel_schlick(0.0f), pbr::fresnel_schlick(0.5f));
	EXPECT_GT(pbr::fresnel_schlick(0.5f), pbr::fresnel_schlick(1.0f));
	EXPECT_FLOAT_EQ(pbr::diffuse_fraction(1.0f), 1.0f - pbr::kDielectricF0);
	EXPECT_FLOAT_EQ(pbr::diffuse_fraction(0.0f), 0.0f);
}

TEST(PbrToneMapTest, AcesIsBoundedAndMonotonic)
{
	EXPECT_FLOAT_EQ(pbr::aces(0.0f), 0.0f);
	float previous = 0.0f;
	for (const float x : {0.1f, 0.5f, 1.0f, 2.0f}) {
		const float mapped = pbr::aces(x);
		EXPECT_GE(mapped, 0.0f);
		EXPECT_LE(mapped, 1.0f);
		EXPECT_GT(mapped, previous) << "x=" << x;
		previous = mapped;
	}
	for (const float x : {8.0f, 100.0f}) {
		const float mapped = pbr::aces(x);
		EXPECT_GE(mapped, 0.0f);
		EXPECT_LE(mapped, 1.0f);
		EXPECT_GE(mapped, previous) << "x=" << x;
		previous = mapped;
	}
}

TEST(PbrToneMapTest, SrgbRoundTripPreservesAlbedo)
{
	for (const float c : {0.0f, 0.01f, 0.18f, 0.5f, 1.0f}) {
		EXPECT_NEAR(pbr::linear_to_srgb(pbr::srgb_to_linear(c)), c, 2e-3f)
		        << "c=" << c;
	}
}

TEST(PbrLightingTest, ExposureDefaultsToNeutral)
{
	EXPECT_FLOAT_EQ(LightingSettings{}.exposure, 1.0f);
	EXPECT_FLOAT_EQ(SceneSettings{}.exposure, 1.0f);
}

TEST(PbrHemisphereTest, DefaultsReproduceLegacyFloorExactly)
{
	// Sky/ground default to the reference gray, so the hemisphere multiplier
	// is 1.0 and an old document renders its legacy ambient bit-for-bit --
	// for ANY custom ambient, not just the default.
	for (const glm::vec3 ambient :
	     {glm::vec3{0.15f, 0.15f, 0.18f}, glm::vec3{0.4f, 0.3f, 0.2f},
	      glm::vec3{0.02f}}) {
		for (const float normal_y : {-1.0f, -0.3f, 0.0f, 0.5f, 1.0f}) {
			const glm::vec3 result = pbr::hemisphere_ambient(
			        ambient, pbr::kHemiReference, pbr::kHemiReference,
			        normal_y);
			EXPECT_FLOAT_EQ(result.x, ambient.x)
			        << "normal_y=" << normal_y;
			EXPECT_FLOAT_EQ(result.y, ambient.y)
			        << "normal_y=" << normal_y;
			EXPECT_FLOAT_EQ(result.z, ambient.z)
			        << "normal_y=" << normal_y;
		}
	}
}

TEST(PbrHemisphereTest, BlendFollowsWorldNormalY)
{
	EXPECT_FLOAT_EQ(pbr::hemisphere_blend(-1.0f), 0.0f);
	EXPECT_FLOAT_EQ(pbr::hemisphere_blend(0.0f), 0.5f);
	EXPECT_FLOAT_EQ(pbr::hemisphere_blend(1.0f), 1.0f);
	EXPECT_FLOAT_EQ(pbr::hemisphere_blend(-2.0f), 0.0f); // clamped
	EXPECT_FLOAT_EQ(pbr::hemisphere_blend(2.0f), 1.0f); // clamped

	// Facing straight up yields sky, straight down yields ground (with a
	// default ambient the colours apply verbatim).
	const glm::vec3 ambient{0.15f, 0.15f, 0.18f};
	const glm::vec3 sky{0.25f, 0.35f, 0.5f};
	const glm::vec3 ground{0.1f, 0.08f, 0.06f};
	const glm::vec3 up =
	        pbr::hemisphere_ambient(ambient, sky, ground, 1.0f);
	const glm::vec3 down =
	        pbr::hemisphere_ambient(ambient, sky, ground, -1.0f);
	EXPECT_NEAR(up.x, sky.x, 1e-6f);
	EXPECT_NEAR(up.y, sky.y, 1e-6f);
	EXPECT_NEAR(up.z, sky.z, 1e-6f);
	EXPECT_NEAR(down.x, ground.x, 1e-6f);
	EXPECT_NEAR(down.y, ground.y, 1e-6f);
	EXPECT_NEAR(down.z, ground.z, 1e-6f);
}

TEST(PbrHemisphereTest, AmbientScalesHemisphereAsGain)
{
	// Doubling ambient doubles the floor at every orientation.
	const glm::vec3 sky{0.25f, 0.35f, 0.5f};
	const glm::vec3 ground{0.1f, 0.08f, 0.06f};
	for (const float normal_y : {-1.0f, 0.0f, 1.0f}) {
		const glm::vec3 one =
		        pbr::hemisphere_ambient({0.2f, 0.2f, 0.2f}, sky, ground,
		                                normal_y);
		const glm::vec3 two =
		        pbr::hemisphere_ambient({0.4f, 0.4f, 0.4f}, sky, ground,
		                                normal_y);
		EXPECT_NEAR(two.x, 2.0f * one.x, 1e-6f);
		EXPECT_NEAR(two.y, 2.0f * one.y, 1e-6f);
		EXPECT_NEAR(two.z, 2.0f * one.z, 1e-6f);
	}
}

TEST(PbrShadowTest, TapRespectsBias)
{
	// Surface clearly in front of the occluder: lit.
	EXPECT_FLOAT_EQ(pbr::shadow_tap(0.5f, 0.8f, 0.0015f), 1.0f);
	// Surface clearly behind: shadowed.
	EXPECT_FLOAT_EQ(pbr::shadow_tap(0.8f, 0.5f, 0.0015f), 0.0f);
	// Within the bias band the surface wins (acne suppression).
	EXPECT_FLOAT_EQ(pbr::shadow_tap(0.501f, 0.5f, 0.0015f), 1.0f);
	// Past the bias band it loses.
	EXPECT_FLOAT_EQ(pbr::shadow_tap(0.502f, 0.5f, 0.0015f), 0.0f);
}

TEST(PbrShadowTest, PcfAverageStaysInRange)
{
	// A 3x3 neighbourhood average is a valid occlusion factor: 1 all lit, 0
	// all shadowed, strictly between on an edge.
	float all_lit = 0.0f, all_dark = 0.0f, edge = 0.0f;
	for (int i = 0; i < 9; ++i) {
		all_lit += pbr::shadow_tap(0.5f, 0.8f, 0.0015f);
		all_dark += pbr::shadow_tap(0.8f, 0.5f, 0.0015f);
		edge += pbr::shadow_tap(i < 5 ? 0.5f : 0.8f, 0.65f, 0.0015f);
	}
	EXPECT_FLOAT_EQ(all_lit / 9.0f, 1.0f);
	EXPECT_FLOAT_EQ(all_dark / 9.0f, 0.0f);
	EXPECT_GT(edge / 9.0f, 0.0f);
	EXPECT_LT(edge / 9.0f, 1.0f);
}

TEST(PbrLightingTest, NewSettingsDefaultOnAndMatchAmbient)
{
	const LightingSettings lighting;
	EXPECT_TRUE(lighting.shadows_enabled);
	EXPECT_TRUE(lighting.fxaa_enabled);
	EXPECT_FLOAT_EQ(lighting.sky_color.x, lighting.ambient.x);
	EXPECT_FLOAT_EQ(lighting.sky_color.y, lighting.ambient.y);
	EXPECT_FLOAT_EQ(lighting.sky_color.z, lighting.ambient.z);
	EXPECT_FLOAT_EQ(lighting.ground_color.x, lighting.ambient.x);
	EXPECT_FLOAT_EQ(lighting.ground_color.y, lighting.ambient.y);
	EXPECT_FLOAT_EQ(lighting.ground_color.z, lighting.ambient.z);

	const SceneSettings scene;
	EXPECT_TRUE(scene.shadows_enabled);
	EXPECT_TRUE(scene.fxaa_enabled);
	EXPECT_FLOAT_EQ(scene.sky_color.x, scene.ambient.x);
	EXPECT_FLOAT_EQ(scene.sky_color.y, scene.ambient.y);
	EXPECT_FLOAT_EQ(scene.sky_color.z, scene.ambient.z);
	EXPECT_FLOAT_EQ(scene.ground_color.x, scene.ambient.x);
	EXPECT_FLOAT_EQ(scene.ground_color.y, scene.ambient.y);
	EXPECT_FLOAT_EQ(scene.ground_color.z, scene.ambient.z);
}
