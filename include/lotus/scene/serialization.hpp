#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lotus/ecs.hpp"

namespace lotus {

class Scene;

/// Version of the on-disk scene format. Bump when the schema changes.
inline constexpr int kSceneFormatVersion = 1;

/// Maps component type names to JSON writers/readers, so adding a new
/// component needs one registration instead of another branch in a parser.
class ComponentCodecs {
public:
	/// Returns false when the entity does not carry the component.
	using WriteFn = std::function<bool(const Registry&, Entity, nlohmann::json&)>;
	using ReadFn  = std::function<void(Registry&, Entity, const nlohmann::json&)>;

	void add(std::string name, WriteFn write, ReadFn read);
	const WriteFn* writer(const std::string& name) const;
	const ReadFn* reader(const std::string& name) const;

	/// Registered names, in deterministic (sorted) order.
	const std::vector<std::string>& names() const { return order_; }

private:
	struct Codec {
		WriteFn write;
		ReadFn read;
	};
	std::map<std::string, Codec> codecs_;
	std::vector<std::string> order_;
};

/// Built-in codecs: Transform, Sprite, Camera2D.
/// Name is stored as the entity's "name" field; Hierarchy is derived from
/// nesting; Transform::world is recomputed on load. None are serialized as
/// components.
const ComponentCodecs& default_component_codecs();

/// Serializes a scene (roots, recursively) to a JSON document.
std::string to_json_string(const Scene& scene, int indent = 2);

/// Replaces \p scene from JSON. Parses into a temporary scene first, so a
/// malformed document leaves \p scene untouched. Unknown components are
/// skipped. Returns false and fills \p error on failure.
bool scene_from_json(Scene& scene, const std::string& text,
                     std::string* error = nullptr);

bool save_scene(const Scene& scene, const std::string& path,
                std::string* error = nullptr);
bool load_scene(Scene& scene, const std::string& path,
                std::string* error = nullptr);

} // namespace lotus
