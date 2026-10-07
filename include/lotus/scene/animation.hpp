#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

namespace lotus {

enum class AnimationProperty : std::uint8_t {
	Position,
	Rotation,
	Scale,
};

struct AnimationKeyframe {
	float time{0.0f};
	glm::vec3 value{0.0f};
};

struct AnimationTrack {
	SceneGuid target;
	AnimationProperty property{AnimationProperty::Position};
	std::vector<AnimationKeyframe> keyframes;
};

struct AnimationClip {
	std::string name;
	std::vector<AnimationTrack> tracks;
	float duration() const;
};

/// Runtime clip storage keyed by asset id.
class AnimationLibrary {
public:
	/// Loads \p path into \p id. Replaces any previous clip under \p id.
	bool load(const std::string& id, const std::string& path,
	          std::string* error = nullptr);
	const AnimationClip* find(const std::string& id) const;
	void clear();

private:
	std::unordered_map<std::string, AnimationClip> clips_;
};

/// Advances animators and writes target transforms. Call before physics.
void animation_system_update(Scene& scene, AnimationLibrary& library,
                             const AssetRegistry* assets, float dt);

bool load_animation_clip_from_file(const std::string& path, AnimationClip& out,
                                   std::string* error = nullptr);

} // namespace lotus
