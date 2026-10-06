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
inline constexpr int kSceneFormatVersion = 2;

/// Upgrades a parsed document from \p from to \p to in place. Returns false
/// when no upgrade path exists. Called by scene_from_json before any component
/// is read, so a document written by an older version loads unchanged.
bool migrate_scene_json(nlohmann::json& root, int from, int to);

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

/// Built-in codecs, all registered by default: Transform, Sprite, Camera2D,
/// Script, MeshSource, Material, Camera3D, Lod, SceneGuid, EntityRef.
/// Name is stored as the entity's "name" field; Hierarchy is derived from
/// nesting; Transform::world is recomputed on load; EntityRef::resolved is
/// rebuilt by Scene::resolve_refs(). None of those are serialized.
const ComponentCodecs& default_component_codecs();

/// Registers \p name so game code can serialize its own component types.
/// Re-registering a name replaces the previous codec; the built-ins can be
/// overridden this way, which is deliberate. Registration is process-global and
/// not thread-safe: do it once at startup, before any scene is saved or loaded.
void register_component_codec(std::string name, ComponentCodecs::WriteFn write,
                              ComponentCodecs::ReadFn read);

/// Serializes a scene (roots, recursively) to JSON text.
std::string to_json_string(const Scene& scene);

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
