#include "lotus/scene/animation.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "lotus/scene/assets.hpp"

namespace lotus {
namespace {

glm::vec3 lerp(const glm::vec3& a, const glm::vec3& b, float t)
{
	return a + (b - a) * t;
}

glm::vec3 sample_track(const std::vector<AnimationKeyframe>& keys, float time,
                       bool loop, float duration)
{
	if (keys.empty()) {
		return glm::vec3{0.0f};
	}
	if (keys.size() == 1) {
		return keys.front().value;
	}
	float t = time;
	if (loop && duration > 0.0f) {
		t = std::fmod(t, duration);
		if (t < 0.0f) {
			t += duration;
		}
	}
	if (t <= keys.front().time) {
		return keys.front().value;
	}
	if (t >= keys.back().time) {
		return keys.back().value;
	}
	for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
		const AnimationKeyframe& a = keys[i];
		const AnimationKeyframe& b = keys[i + 1];
		if (t >= a.time && t <= b.time) {
			const float span = b.time - a.time;
			const float u    = span > 0.0f ? (t - a.time) / span : 0.0f;
			return lerp(a.value, b.value, u);
		}
	}
	return keys.back().value;
}

AnimationProperty parse_property(const std::string& name)
{
	if (name == "rotation") {
		return AnimationProperty::Rotation;
	}
	if (name == "scale") {
		return AnimationProperty::Scale;
	}
	return AnimationProperty::Position;
}

SceneGuid parse_guid(const nlohmann::json& j)
{
	SceneGuid guid;
	if (j.is_string()) {
		const std::string hex = j.get<std::string>();
		if (!hex.empty()) {
			guid.value = std::stoull(hex, nullptr, 16);
		}
	} else if (j.is_number_unsigned()) {
		guid.value = j.get<uint64_t>();
	}
	return guid;
}

} // namespace

float AnimationClip::duration() const
{
	float end = 0.0f;
	for (const AnimationTrack& track : tracks) {
		for (const AnimationKeyframe& key : track.keyframes) {
			end = std::max(end, key.time);
		}
	}
	return end;
}

bool load_animation_clip_from_file(const std::string& path, AnimationClip& out,
                                   std::string* error)
{
	std::ifstream in(path);
	if (!in) {
		if (error) {
			*error = "cannot open clip: " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	try {
		const nlohmann::json j = nlohmann::json::parse(buffer.str());
		AnimationClip clip;
		if (j.contains("name") && j.at("name").is_string()) {
			clip.name = j.at("name").get<std::string>();
		}
		if (!j.contains("tracks") || !j.at("tracks").is_array()) {
			if (error) {
				*error = "clip has no tracks array";
			}
			return false;
		}
		for (const auto& entry : j.at("tracks")) {
			AnimationTrack track;
			if (entry.contains("guid")) {
				track.target = parse_guid(entry.at("guid"));
			} else if (entry.contains("target")) {
				track.target = parse_guid(entry.at("target"));
			}
			if (entry.contains("property") && entry.at("property").is_string()) {
				track.property =
				        parse_property(entry.at("property").get<std::string>());
			}
			if (!entry.contains("keyframes") ||
			    !entry.at("keyframes").is_array()) {
				continue;
			}
			for (const auto& key : entry.at("keyframes")) {
				AnimationKeyframe frame;
				if (key.contains("t") && key.at("t").is_number()) {
					frame.time = key.at("t").get<float>();
				} else if (key.contains("time") && key.at("time").is_number()) {
					frame.time = key.at("time").get<float>();
				}
				const nlohmann::json* value = nullptr;
				if (key.contains("v") && key.at("v").is_array()) {
					value = &key.at("v");
				} else if (key.contains("value") && key.at("value").is_array()) {
					value = &key.at("value");
				}
				if (value != nullptr && value->size() == 3) {
					frame.value = {(*value)[0].get<float>(),
					               (*value)[1].get<float>(),
					               (*value)[2].get<float>()};
				}
				track.keyframes.push_back(frame);
			}
			std::sort(track.keyframes.begin(), track.keyframes.end(),
			          [](const AnimationKeyframe& a, const AnimationKeyframe& b) {
				          return a.time < b.time;
			          });
			if (!track.keyframes.empty() && track.target.value != 0) {
				clip.tracks.push_back(std::move(track));
			}
		}
		if (clip.tracks.empty()) {
			if (error) {
				*error = "clip contained no usable tracks";
			}
			return false;
		}
		out = std::move(clip);
		return true;
	} catch (const std::exception& ex) {
		if (error) {
			*error = ex.what();
		}
		return false;
	}
}

bool AnimationLibrary::load(const std::string& id, const std::string& path,
                            std::string* error)
{
	AnimationClip clip;
	if (!load_animation_clip_from_file(path, clip, error)) {
		return false;
	}
	if (clip.name.empty()) {
		clip.name = id;
	}
	clips_[id] = std::move(clip);
	return true;
}

const AnimationClip* AnimationLibrary::find(const std::string& id) const
{
	const auto it = clips_.find(id);
	return it != clips_.end() ? &it->second : nullptr;
}

void AnimationLibrary::clear() { clips_.clear(); }

void animation_system_update(Scene& scene, AnimationLibrary& library,
                             const AssetRegistry* assets, float dt)
{
	auto& registry = scene.registry();
	const auto view = registry.view<Animator>();
	for (const Entity entity : view) {
		Animator& animator = view.get<Animator>(entity);
		if (!animator.playing || animator.clip_id.empty()) {
			continue;
		}
		std::string clip_path;
		if (assets != nullptr) {
			clip_path = assets->resolve(animator.clip_id, {});
		}
		if (clip_path.empty()) {
			continue;
		}
		if (library.find(animator.clip_id) == nullptr) {
			std::string error;
			if (!library.load(animator.clip_id, clip_path, &error)) {
				continue;
			}
		}
		const AnimationClip* clip = library.find(animator.clip_id);
		if (!clip) {
			continue;
		}
		const float duration = clip->duration();
		animator.time += dt * animator.speed;
		if (animator.loop && duration > 0.0f) {
			animator.time = std::fmod(animator.time, duration);
			if (animator.time < 0.0f) {
				animator.time += duration;
			}
		} else if (duration > 0.0f && animator.time > duration) {
			animator.time = duration;
			animator.playing = false;
		}

		for (const AnimationTrack& track : clip->tracks) {
			const Entity target = scene.find_by_guid(track.target);
			if (target == NullEntity) {
				continue;
			}
			Transform* transform = registry.try_get<Transform>(target);
			if (!transform) {
				continue;
			}
			const glm::vec3 value =
			        sample_track(track.keyframes, animator.time, animator.loop,
			                     duration);
			switch (track.property) {
			case AnimationProperty::Position:
				transform->position = value;
				break;
			case AnimationProperty::Rotation:
				transform->rotation = value;
				break;
			case AnimationProperty::Scale:
				transform->scale = value;
				break;
			}
		}
	}
}

} // namespace lotus
