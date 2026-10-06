#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "lotus/ecs.hpp"

namespace lotus {

class Scene;

/// A reusable object: a scene fragment in the same versioned JSON format as a
/// scene, so the existing codec round-trips it and there is no second format.
/// The document keeps its nested "children" shape, so hierarchy survives.
struct Prefab {
	std::string name;
	/// The whole fragment document, version and entities included.
	nlohmann::json document;
};

/// Parses a prefab document. Same schema as a scene, including the version
/// field, so a prefab migrates through the same path. Returns false and fills
/// \p error on failure.
bool prefab_from_json(Prefab& prefab, const std::string& text,
                      std::string* error = nullptr);

/// Reads a prefab from disk.
bool load_prefab(Prefab& prefab, const std::string& path,
                 std::string* error = nullptr);

/// Captures \p root and its descendants from \p scene as a prefab.
Prefab prefab_from_scene(const Scene& scene, Entity root);

/// Instantiates \p prefab into \p scene under \p parent, giving every clone a
/// fresh GUID and rewriting internal references to point within the instance.
/// Returns the clone of the fragment's first root, or NullEntity if the prefab
/// is empty. Atomic: a malformed fragment leaves \p scene untouched.
Entity instantiate(const Prefab& prefab, Scene& scene,
                   Entity parent = NullEntity);

/// Merges a scene document into \p target instead of replacing it. Entities
/// already in \p target survive with their GUIDs unchanged; every merged entity
/// gets a fresh GUID and internal references are rewritten to the merged copies.
/// Atomic: a malformed document merges nothing and leaves \p target untouched.
///
/// This is instantiation of a whole document, so it shares the clone-and-remap
/// path with instantiate() rather than growing a second merge implementation.
bool load_scene_additive(Scene& target, const std::string& text,
                         std::string* error = nullptr);

/// Reads \p path and merges it into \p target. See load_scene_additive.
bool load_scene_additive_from_file(Scene& target, const std::string& path,
                                   std::string* error = nullptr);

} // namespace lotus
