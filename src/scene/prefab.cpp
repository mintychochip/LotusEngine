#include "lotus/scene/prefab.hpp"

#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>

#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

namespace lotus {
namespace {

std::string guid_to_hex(SceneGuid guid)
{
	std::ostringstream out;
	out << std::hex << guid.value;
	return out.str();
}

/// Depth-first search for the entity node whose SceneGuid matches \p hex.
const nlohmann::json* find_by_guid(const nlohmann::json& entities,
                                   const std::string& hex)
{
	for (const auto& entity : entities) {
		if (!entity.is_object()) {
			continue;
		}
		if (entity.contains("components")) {
			const auto& components = entity.at("components");
			if (components.contains("SceneGuid") &&
			    components.at("SceneGuid").is_string() &&
			    components.at("SceneGuid").get<std::string>() == hex) {
				return &entity;
			}
		}
		if (entity.contains("children")) {
			if (const nlohmann::json* found =
			            find_by_guid(entity.at("children"), hex)) {
				return found;
			}
		}
	}
	return nullptr;
}

} // namespace

bool prefab_from_json(Prefab& prefab, const std::string& text,
                      std::string* error)
{
	const auto fail = [error](const std::string& message) {
		if (error) {
			*error = message;
		}
		return false;
	};

	nlohmann::json root;
	try {
		root = nlohmann::json::parse(text);
	} catch (const std::exception& ex) {
		return fail(std::string{"prefab JSON parse error: "} + ex.what());
	}
	if (!root.is_object()) {
		return fail("prefab document must be a JSON object");
	}

	const int version = root.value("version", 0);
	if (version <= 0) {
		return fail("prefab document has no version");
	}
	if (version > kSceneFormatVersion) {
		return fail("unsupported prefab version: " + std::to_string(version));
	}
	if (!migrate_scene_json(root, version, kSceneFormatVersion)) {
		return fail("cannot migrate prefab from version " +
		            std::to_string(version));
	}
	if (root.contains("entities") && !root.at("entities").is_array()) {
		return fail("prefab 'entities' must be an array");
	}

	Prefab parsed;
	parsed.name     = root.value("name", std::string{});
	parsed.document = std::move(root);
	prefab          = std::move(parsed);
	return true;
}

bool load_prefab(Prefab& prefab, const std::string& path, std::string* error)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		if (error) {
			*error = "cannot open prefab: " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	return prefab_from_json(prefab, buffer.str(), error);
}

Prefab prefab_from_scene(const Scene& scene, Entity root)
{
	Prefab prefab;
	if (root == NullEntity || !scene.registry().valid(root)) {
		return prefab;
	}
	// Serialize the scene with the same writer a save uses, then lift the
	// subtree out by GUID. Matching on identity rather than traversal order
	// matters: roots() iterates a view, so its order is not the document's.
	const nlohmann::json document =
	        nlohmann::json::parse(to_json_string(scene));
	if (!document.contains("entities")) {
		return prefab;
	}
	const nlohmann::json* subtree =
	        find_by_guid(document.at("entities"), guid_to_hex(scene.guid_of(root)));
	if (subtree == nullptr) {
		return prefab;
	}

	prefab.name = scene.registry().get<Name>(root).value;
	prefab.document = nlohmann::json{{"version", kSceneFormatVersion},
	                                 {"name", prefab.name},
	                                 {"entities", nlohmann::json::array({*subtree})}};
	return prefab;
}

namespace {

/// Clones every entity of \p staging into \p scene, assigning fresh GUIDs and
/// rewriting internal references to the clones. Returns the first clone in
/// staging's root order, or NullEntity when staging is empty.
///
/// This is the one seam `instantiate` and `load_scene_additive` share. A second
/// copy of this logic would be the bug: identity and reference remapping are
/// exactly the parts that are easy to get subtly wrong.
Entity clone_all_into(const Scene& staging, Scene& scene)
{
	const ComponentCodecs& codecs = default_component_codecs();

	// Pass 1: clone every entity, copying components generically through the
	// codec so anything registered survives. SceneGuid is deliberately skipped:
	// the clone's identity must be new, or two instances would share GUIDs.
	// EntityRef targets are rewritten in pass 2b.
	std::unordered_map<Entity, Entity> clones;
	std::unordered_map<uint64_t, SceneGuid> guid_map;
	for (const Entity source : staging.registry().view<Name>()) {
		const Entity clone =
		        scene.create(staging.registry().get<Name>(source).value);
		clones[source] = clone;
		guid_map[staging.guid_of(source).value] = scene.guid_of(clone);

		for (const std::string& name : codecs.names()) {
			if (name == "SceneGuid" || name == "EntityRef") {
				continue;
			}
			const auto* write = codecs.writer(name);
			const auto* read  = codecs.reader(name);
			if (write == nullptr || read == nullptr) {
				continue;
			}
			nlohmann::json value;
			if ((*write)(staging.registry(), source, value)) {
				(*read)(scene.registry(), clone, value);
			}
		}
	}

	// Pass 2a: hierarchy, so children are not left as siblings.
	for (const Entity source : staging.registry().view<Hierarchy>()) {
		const Entity source_parent =
		        staging.registry().get<Hierarchy>(source).parent;
		if (source_parent != NullEntity) {
			scene.set_parent(clones.at(source), clones.at(source_parent));
		}
	}

	// Pass 2b: references, rewritten to the clones' GUIDs. A target that
	// pointed outside the fragment is left alone; resolve_refs reports it.
	for (const Entity source : staging.registry().view<EntityRef>()) {
		const auto* source_ref = staging.registry().try_get<EntityRef>(source);
		if (source_ref == nullptr) {
			continue;
		}
		EntityRef clone_ref{};
		const auto mapped = guid_map.find(source_ref->target.value);
		clone_ref.target = mapped != guid_map.end() ? mapped->second
		                                            : source_ref->target;
		scene.registry().emplace_or_replace<EntityRef>(clones.at(source),
		                                              clone_ref);
	}

	Entity first = NullEntity;
	for (const Entity source : staging.roots()) {
		const auto it = clones.find(source);
		if (it != clones.end()) {
			first = it->second;
			break;
		}
	}
	return first;
}

} // namespace

Entity instantiate(const Prefab& prefab, Scene& scene, Entity parent)
{
	if (!prefab.document.contains("entities") ||
	    !prefab.document.at("entities").is_array() ||
	    prefab.document.at("entities").empty()) {
		return NullEntity;
	}

	// Build the fragment in isolation first. scene_from_json is atomic and
	// validates the whole document, so a malformed prefab returns here with the
	// target scene untouched.
	Scene staging{"prefab"};
	std::string error;
	if (!scene_from_json(staging, prefab.document.dump(), &error)) {
		return NullEntity;
	}

	const Entity first = clone_all_into(staging, scene);
	if (parent != NullEntity && first != NullEntity) {
		scene.set_parent(first, parent);
	}
	scene.resolve_refs();
	return first;
}

bool load_scene_additive(Scene& target, const std::string& text,
                         std::string* error)
{
	// Parse and validate the whole document before touching the target, so a
	// malformed file merges nothing. Same atomicity contract as scene_from_json,
	// with merge instead of replace.
	Scene staging{"merge"};
	if (!scene_from_json(staging, text, error)) {
		return false;
	}
	clone_all_into(staging, target);
	target.resolve_refs();
	return true;
}

bool load_scene_additive_from_file(Scene& target, const std::string& path,
                                   std::string* error)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		if (error) {
			*error = "cannot open scene: " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	return load_scene_additive(target, buffer.str(), error);
}

} // namespace lotus
