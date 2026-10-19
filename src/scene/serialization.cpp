#include "lotus/scene/serialization.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "lotus/scene/scene.hpp"
#include "lotus/script/script.hpp"

namespace lotus {

// --- helpers ---------------------------------------------------------------

namespace {

/// Rounds to 6 decimals so saved scenes stay readable (0.9, not
/// 0.8999999761581421) for humans and AI agents editing the file.
double tidy(float value)
{
	return std::round(static_cast<double>(value) * 1e6) / 1e6;
}

nlohmann::json arr2(float x, float y)
{
	return nlohmann::json::array({tidy(x), tidy(y)});
}

nlohmann::json arr3(float x, float y, float z)
{
	return nlohmann::json::array({tidy(x), tidy(y), tidy(z)});
}

nlohmann::json arr4(float x, float y, float z, float w)
{
	return nlohmann::json::array({tidy(x), tidy(y), tidy(z), tidy(w)});
}

} // namespace

// --- component converters (namespace lotus, so ADL finds them) --------------

void to_json(nlohmann::json& j, const Name& value)
{
	j = nlohmann::json{{"value", value.value}};
}

void from_json(const nlohmann::json& j, Name& value)
{
	if (j.is_object() && j.contains("value") && j.at("value").is_string()) {
		value.value = j.at("value").get<std::string>();
	}
}

// GUIDs are written as hex strings, not numbers: a JSON number is a double in
// most readers, so a value above 2^53 would silently lose precision.
void to_json(nlohmann::json& j, const SceneGuid& guid)
{
	std::ostringstream out;
	out << std::hex << guid.value;
	j = out.str();
}

void from_json(const nlohmann::json& j, SceneGuid& guid)
{
	if (j.is_string()) {
		try {
			guid.value = std::stoull(j.get<std::string>(), nullptr, 16);
		} catch (const std::exception&) {
			guid.value = 0; // unparseable: treat as unassigned, regenerate
		}
	}
}

// Only the target is written; \p resolved is derived and rebuilt on load.
void to_json(nlohmann::json& j, const EntityRef& ref)
{
	j = nlohmann::json{{"target", ref.target}};
}

void from_json(const nlohmann::json& j, EntityRef& ref)
{
	if (j.is_object() && j.contains("target")) {
		ref.target = j.at("target").get<SceneGuid>();
	}
	ref.resolved = NullEntity;
}

void to_json(nlohmann::json& j, const Transform& t)
{
	j = nlohmann::json{
	        {"position", arr3(t.position.x, t.position.y, t.position.z)},
	        {"rotation", arr3(t.rotation.x, t.rotation.y, t.rotation.z)},
	        {"scale", arr3(t.scale.x, t.scale.y, t.scale.z)}};
}

void from_json(const nlohmann::json& j, Transform& t)
{
	const auto read3 = [&](const char* key, glm::vec3& out) {
		if (!j.contains(key) || !j.at(key).is_array() || j.at(key).size() != 3) {
			return;
		}
		const auto& a = j.at(key);
		out = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
	};
	read3("position", t.position);
	read3("rotation", t.rotation);
	read3("scale", t.scale);
}

void to_json(nlohmann::json& j, const Sprite& s)
{
	j = nlohmann::json{
	        {"color", arr4(s.color.r, s.color.g, s.color.b, s.color.a)},
	        {"size", arr2(s.size.x, s.size.y)}};
}

void from_json(const nlohmann::json& j, Sprite& s)
{
	if (j.contains("color") && j.at("color").is_array() &&
	    j.at("color").size() == 4) {
		const auto& c = j.at("color");
		s.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(),
		           c[3].get<float>()};
	}
	if (j.contains("size") && j.at("size").is_array() && j.at("size").size() == 2) {
		const auto& v = j.at("size");
		s.size = {v[0].get<float>(), v[1].get<float>()};
	}
}

void to_json(nlohmann::json& j, const Camera2D& c)
{
	j = nlohmann::json{{"position", arr2(c.position.x, c.position.y)},
	                   {"zoom", tidy(c.zoom)}};
}

void from_json(const nlohmann::json& j, Camera2D& c)
{
	if (j.contains("position") && j.at("position").is_array() &&
	    j.at("position").size() == 2) {
		const auto& p = j.at("position");
		c.position = {p[0].get<float>(), p[1].get<float>()};
	}
	if (j.contains("zoom") && j.at("zoom").is_number()) {
		c.zoom = j.at("zoom").get<float>();
	}
}

void to_json(nlohmann::json& j, const Script& s)
{
	j = nlohmann::json{{"path", s.path}};
	if (!s.id.empty()) {
		j["id"] = s.id;
	}
}

void from_json(const nlohmann::json& j, Script& s)
{
	if (!j.is_object()) {
		return;
	}
	if (j.contains("path") && j.at("path").is_string()) {
		s.path = j.at("path").get<std::string>();
	}
	// v3: optional stable asset key. Absent in a migrated v2 document, which
	// means "resolve by path" -- exactly the pre-v3 behaviour.
	if (j.contains("id") && j.at("id").is_string()) {
		s.id = j.at("id").get<std::string>();
	}
}

void to_json(nlohmann::json& j, const MeshSource& m)
{
	j = nlohmann::json{{"path", m.path}};
	if (!m.id.empty()) {
		j["id"] = m.id;
	}
}

void from_json(const nlohmann::json& j, MeshSource& m)
{
	if (!j.is_object()) {
		return;
	}
	if (j.contains("path") && j.at("path").is_string()) {
		m.path = j.at("path").get<std::string>();
	}
	if (j.contains("id") && j.at("id").is_string()) {
		m.id = j.at("id").get<std::string>();
	}
}

void to_json(nlohmann::json& j, const Material& m)
{
	j = nlohmann::json{{"base_color", arr3(m.base_color.x, m.base_color.y,
	                                       m.base_color.z)},
	                   {"roughness", tidy(m.roughness)}};
	if (!m.albedo_id.empty()) {
		j["albedo_id"] = m.albedo_id;
	}
}


void to_json(nlohmann::json& j, const RigidBody& body)
{
	const char* motion = "dynamic";
	switch (body.motion_type) {
	case PhysicsMotionType::Static:
		motion = "static";
		break;
	case PhysicsMotionType::Kinematic:
		motion = "kinematic";
		break;
	case PhysicsMotionType::Dynamic:
		break;
	}
	j = nlohmann::json{{"motion_type", motion},
	                   {"mass", tidy(body.mass)},
	                   {"friction", tidy(body.friction)},
	                   {"restitution", tidy(body.restitution)}};
}

void from_json(const nlohmann::json& j, RigidBody& body)
{
	if (j.contains("motion_type") && j.at("motion_type").is_string()) {
		const std::string motion = j.at("motion_type").get<std::string>();
		if (motion == "static") {
			body.motion_type = PhysicsMotionType::Static;
		} else if (motion == "kinematic") {
			body.motion_type = PhysicsMotionType::Kinematic;
		} else {
			body.motion_type = PhysicsMotionType::Dynamic;
		}
	}
	if (j.contains("mass") && j.at("mass").is_number()) {
		body.mass = j.at("mass").get<float>();
	}
	if (j.contains("friction") && j.at("friction").is_number()) {
		body.friction = j.at("friction").get<float>();
	}
	if (j.contains("restitution") && j.at("restitution").is_number()) {
		body.restitution = j.at("restitution").get<float>();
	}
}

void to_json(nlohmann::json& j, const BoxCollider& box)
{
	j = nlohmann::json{
	        {"half_extents",
	         arr3(box.half_extents.x, box.half_extents.y, box.half_extents.z)}};
}

void from_json(const nlohmann::json& j, BoxCollider& box)
{
	if (j.contains("half_extents") && j.at("half_extents").is_array() &&
	    j.at("half_extents").size() == 3) {
		const auto& e = j.at("half_extents");
		box.half_extents = {e[0].get<float>(), e[1].get<float>(),
		                    e[2].get<float>()};
	}
}

void to_json(nlohmann::json& j, const SphereCollider& sphere)
{
	j = nlohmann::json{{"radius", tidy(sphere.radius)}};
}

void from_json(const nlohmann::json& j, SphereCollider& sphere)
{
	if (j.contains("radius") && j.at("radius").is_number()) {
		sphere.radius = j.at("radius").get<float>();
	}
}

void to_json(nlohmann::json& j, const MeshCollider&)
{
	j = nlohmann::json::object();
}

void from_json(const nlohmann::json&, MeshCollider&) {}

void to_json(nlohmann::json& j, const CharacterController& controller)
{
	j = nlohmann::json{{"radius", tidy(controller.radius)},
	                   {"half_height", tidy(controller.half_height)}};
}

void from_json(const nlohmann::json& j, CharacterController& controller)
{
	if (j.contains("radius") && j.at("radius").is_number()) {
		controller.radius = j.at("radius").get<float>();
	}
	if (j.contains("half_height") && j.at("half_height").is_number()) {
		controller.half_height = j.at("half_height").get<float>();
	}
}

void to_json(nlohmann::json& j, const Animator& animator)
{
	j = nlohmann::json{{"time", tidy(animator.time)},
	                   {"speed", tidy(animator.speed)},
	                   {"loop", animator.loop},
	                   {"playing", animator.playing}};
	if (!animator.clip_id.empty()) {
		j["clip_id"] = animator.clip_id;
	}
}

void from_json(const nlohmann::json& j, Animator& animator)
{
	if (j.contains("clip_id") && j.at("clip_id").is_string()) {
		animator.clip_id = j.at("clip_id").get<std::string>();
	}
	if (j.contains("time") && j.at("time").is_number()) {
		animator.time = j.at("time").get<float>();
	}
	if (j.contains("speed") && j.at("speed").is_number()) {
		animator.speed = j.at("speed").get<float>();
	}
	if (j.contains("loop") && j.at("loop").is_boolean()) {
		animator.loop = j.at("loop").get<bool>();
	}
	if (j.contains("playing") && j.at("playing").is_boolean()) {
		animator.playing = j.at("playing").get<bool>();
	}
}

void from_json(const nlohmann::json& j, Material& m)
{
	if (j.contains("base_color") && j.at("base_color").is_array() &&
	    j.at("base_color").size() == 3) {
		const auto& c = j.at("base_color");
		m.base_color  = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
	}
	if (j.contains("albedo_id") && j.at("albedo_id").is_string()) {
		m.albedo_id = j.at("albedo_id").get<std::string>();
	}
	if (j.contains("roughness") && j.at("roughness").is_number()) {
		m.roughness = j.at("roughness").get<float>();
	}
}

void to_json(nlohmann::json& j, const Camera3D& c)
{
	j = nlohmann::json{
	        {"position", arr3(c.position.x, c.position.y, c.position.z)},
	        {"target", arr3(c.target.x, c.target.y, c.target.z)},
	        {"fov_degrees", tidy(c.fov_degrees)},
	        {"near_plane", tidy(c.near_plane)},
	        {"far_plane", tidy(c.far_plane)},
	        {"follow", c.follow}};
}

void from_json(const nlohmann::json& j, Camera3D& c)
{
	const auto read3 = [&](const char* key, glm::vec3& out) {
		if (!j.contains(key) || !j.at(key).is_array() || j.at(key).size() != 3) {
			return;
		}
		const auto& a = j.at(key);
		out = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
	};
	read3("position", c.position);
	read3("target", c.target);
	if (j.contains("fov_degrees") && j.at("fov_degrees").is_number()) {
		c.fov_degrees = j.at("fov_degrees").get<float>();
	}
	if (j.contains("near_plane") && j.at("near_plane").is_number()) {
		c.near_plane = j.at("near_plane").get<float>();
	}
	if (j.contains("far_plane") && j.at("far_plane").is_number()) {
		c.far_plane = j.at("far_plane").get<float>();
	}
	if (j.contains("follow") && j.at("follow").is_boolean()) {
		c.follow = j.at("follow").get<bool>();
	}
}

void to_json(nlohmann::json& j, const Lod& lod)
{
	nlohmann::json levels = nlohmann::json::array();
	for (const LodLevel& level : lod.levels) {
		levels.push_back(nlohmann::json{{"source", level.source},
		                                {"max_distance", tidy(level.max_distance)}});
	}
	j = nlohmann::json{{"levels", std::move(levels)}};
}

void from_json(const nlohmann::json& j, Lod& lod)
{
	lod.levels.clear();
	if (!j.contains("levels") || !j.at("levels").is_array()) {
		return;
	}
	for (const auto& entry : j.at("levels")) {
		LodLevel level;
		if (entry.contains("source") && entry.at("source").is_string()) {
			level.source = entry.at("source").get<std::string>();
		}
		if (entry.contains("max_distance") && entry.at("max_distance").is_number()) {
			level.max_distance = entry.at("max_distance").get<float>();
		}
		lod.levels.push_back(std::move(level));
	}
}

// --- codec registry --------------------------------------------------------

void ComponentCodecs::add(std::string name, WriteFn write, ReadFn read)
{
	if (codecs_.find(name) == codecs_.end()) {
		order_.push_back(name);
	}
	codecs_[std::move(name)] = Codec{std::move(write), std::move(read)};
	std::sort(order_.begin(), order_.end());
}

const ComponentCodecs::WriteFn* ComponentCodecs::writer(const std::string& name) const
{
	const auto it = codecs_.find(name);
	return it == codecs_.end() ? nullptr : &it->second.write;
}

const ComponentCodecs::ReadFn* ComponentCodecs::reader(const std::string& name) const
{
	const auto it = codecs_.find(name);
	return it == codecs_.end() ? nullptr : &it->second.read;
}

namespace {

/// The one registry instance. Registration mutates it, so it is not const;
/// registration happens at startup, so no synchronisation is needed.
ComponentCodecs& mutable_component_codecs()
{
	static ComponentCodecs codecs = [] {
		ComponentCodecs c;
		c.add(
		        "Transform",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Transform>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Transform>(e, j.get<Transform>());
		        });
		c.add(
		        "Sprite",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Sprite>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Sprite>(e, j.get<Sprite>());
		        });
		c.add(
		        "Camera2D",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Camera2D>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Camera2D>(e, j.get<Camera2D>());
		        });
		c.add(
		        "Script",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Script>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Script>(e, j.get<Script>());
		        });
		c.add(
		        "MeshSource",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<MeshSource>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<MeshSource>(e, j.get<MeshSource>());
		        });
		c.add(
		        "Material",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Material>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Material>(e, j.get<Material>());
		        });
		c.add(
		        "SceneGuid",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<SceneGuid>(e);
			        if (!value || value->value == 0) {
				        return false; // unassigned: omit, it is regenerated
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<SceneGuid>(e, j.get<SceneGuid>());
		        });
		c.add(
		        "EntityRef",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<EntityRef>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<EntityRef>(e, j.get<EntityRef>());
		        });
		c.add(
		        "SceneOrigin",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<SceneOrigin>(e);
			        if (!value || value->scene.empty()) {
				        return false; // unassigned: omit
			        }
			        j = nlohmann::json{{"scene", value->scene}};
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        SceneOrigin origin;
			        if (j.is_object() && j.contains("scene") &&
			            j.at("scene").is_string()) {
				        origin.scene = j.at("scene").get<std::string>();
			        }
			        r.emplace_or_replace<SceneOrigin>(e, std::move(origin));
		        });
		c.add(
		        "Camera3D",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Camera3D>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Camera3D>(e, j.get<Camera3D>());
		        });
		c.add(
		        "Lod",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Lod>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Lod>(e, j.get<Lod>());
		        });
		c.add(
		        "RigidBody",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<RigidBody>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<RigidBody>(e, j.get<RigidBody>());
		        });
		c.add(
		        "BoxCollider",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<BoxCollider>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<BoxCollider>(e, j.get<BoxCollider>());
		        });
		c.add(
		        "SphereCollider",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<SphereCollider>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<SphereCollider>(e,
			                                             j.get<SphereCollider>());
		        });
		c.add(
		        "MeshCollider",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        if (!r.all_of<MeshCollider>(e)) {
				        return false;
			        }
			        j = nlohmann::json::object();
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        (void)j;
			        r.emplace_or_replace<MeshCollider>(e);
		        });
		c.add(
		        "CharacterController",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<CharacterController>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<CharacterController>(
			                e, j.get<CharacterController>());
		        });
		c.add(
		        "Animator",
		        [](const Registry& r, Entity e, nlohmann::json& j) {
			        const auto* value = r.try_get<Animator>(e);
			        if (!value) {
				        return false;
			        }
			        j = *value;
			        return true;
		        },
		        [](Registry& r, Entity e, const nlohmann::json& j) {
			        r.emplace_or_replace<Animator>(e, j.get<Animator>());
		        });
		return c;
	}();
	return codecs;
}

} // namespace

const ComponentCodecs& default_component_codecs()
{
	return mutable_component_codecs();
}

void register_component_codec(std::string name, ComponentCodecs::WriteFn write,
                              ComponentCodecs::ReadFn read)
{
	mutable_component_codecs().add(std::move(name), std::move(write),
	                               std::move(read));
}

// --- scene -> text ---------------------------------------------------------

namespace {

/// Compact single-line JSON. Keeps vectors inline so a Transform is one line
/// instead of twelve, which matters for hand- and AI-authored diffs.
std::string dump_compact(const nlohmann::json& value)
{
	switch (value.type()) {
	case nlohmann::json::value_t::null:
		return "null";
	case nlohmann::json::value_t::boolean:
		return value.get<bool>() ? "true" : "false";
	case nlohmann::json::value_t::string:
		return value.dump();
	case nlohmann::json::value_t::number_integer:
		return std::to_string(value.get<long long>());
	case nlohmann::json::value_t::number_unsigned:
		return std::to_string(value.get<unsigned long long>());
	case nlohmann::json::value_t::number_float:
		return value.dump();
	case nlohmann::json::value_t::array: {
		std::string out = "[";
		for (std::size_t i = 0; i < value.size(); ++i) {
			if (i != 0) {
				out += ", ";
			}
			out += dump_compact(value[i]);
		}
		return out + "]";
	}
	case nlohmann::json::value_t::object: {
		std::string out = "{";
		bool first      = true;
		for (auto it = value.begin(); it != value.end(); ++it) {
			if (!first) {
				out += ", ";
			}
			first = false;
			out += nlohmann::json(it.key()).dump();
			out += ": ";
			out += dump_compact(it.value());
		}
		return out + "}";
	}
	default:
		return value.dump();
	}
}

void write_entity(std::ostringstream& out, const Registry& registry,
                  Entity entity, const ComponentCodecs& codecs,
                  const std::string& indent)
{
	const std::string inner = indent + "  ";
	out << indent << "{\n";
	out << inner << "\"name\": "
	    << nlohmann::json(registry.get<Name>(entity).value).dump();

	nlohmann::json components = nlohmann::json::object();
	for (const std::string& name : codecs.names()) {
		const auto* write = codecs.writer(name);
		nlohmann::json value;
		if (write && (*write)(registry, entity, value)) {
			components[name] = std::move(value);
		}
	}
	if (!components.empty()) {
		out << ",\n" << inner << "\"components\": {\n";
		bool first = true;
		for (auto it = components.begin(); it != components.end(); ++it) {
			if (!first) {
				out << ",\n";
			}
			first = false;
			out << inner << "  " << nlohmann::json(it.key()).dump() << ": "
			    << dump_compact(it.value());
		}
		out << "\n" << inner << "}";
	}

	const auto& hierarchy = registry.get<Hierarchy>(entity);
	if (hierarchy.first_child != NullEntity) {
		out << ",\n" << inner << "\"children\": [\n";
		bool first = true;
		for (Entity child = hierarchy.first_child; child != NullEntity;
		     child         = registry.get<Hierarchy>(child).next_sibling) {
			if (!first) {
				out << ",\n";
			}
			first = false;
			write_entity(out, registry, child, codecs, inner + "  ");
		}
		out << "\n" << inner << "]";
	}
	out << "\n" << indent << "}";
}

} // namespace

// --- json -> scene ---------------------------------------------------------

namespace {

Entity entity_from_json(Scene& scene, const nlohmann::json& j,
                        const ComponentCodecs& codecs,
                        std::vector<std::string>* warnings)
{
	const std::string name = j.value("name", std::string{});
	const Entity entity    = scene.create(name);

	if (j.contains("components")) {
		const auto& components = j.at("components");
		for (const auto& [key, value] : components.items()) {
			const auto* read = codecs.reader(key);
			if (!read) {
				if (warnings) {
					warnings->push_back("unknown component: " + key);
				}
				continue;
			}
			(*read)(scene.registry(), entity, value);
		}
	}

	if (j.contains("children")) {
		const auto& children = j.at("children");
		if (!children.is_array()) {
			throw std::runtime_error("'children' must be an array");
		}
		for (const auto& child : children) {
			if (!child.is_object()) {
				throw std::runtime_error("child entity must be an object");
			}
			const Entity child_entity =
			        entity_from_json(scene, child, codecs, warnings);
			scene.set_parent(child_entity, entity);
		}
	}
	return entity;
}

} // namespace

bool migrate_scene_json(nlohmann::json& root, int from, int to)
{
	if (from == to) {
		return true;
	}
	if (from < 1 || from > to) {
		return false; // nothing below version 1, and no downgrade path
	}
	// One step at a time, so a future schema change has a home here instead of
	// turning every version bump into a breaking change.
	// 2 -> 3: MeshSource and Script gain an optional `id` (asset indirection),
	// and a document may carry a top-level `settings` block. Both are additive:
	// an absent `id` means "use the path", and an absent `settings` means "use
	// the renderer's globals". So there is nothing to rewrite.
	for (int version = from; version < to; ++version) {
		switch (version) {
		case 1: // 1 -> 2 changed no fields
		case 2: // 2 -> 3 is additive
			break;
		default:
			return false;
		}
	}
	root["version"] = to;
	return true;
}

std::string to_json_string(const Scene& scene)
{
	const ComponentCodecs& codecs = default_component_codecs();
	std::ostringstream out;
	out << "{\n";
	out << "  \"version\": " << kSceneFormatVersion << ",\n";
	out << "  \"name\": " << nlohmann::json(scene.name()).dump() << ",\n";
	// Written only when the scene actually carries one, so a document without
	// settings leaves the renderer's globals in place on load.
	if (scene.has_settings()) {
		const SceneSettings& s = scene.settings();
		out << "  \"settings\": {"
		    << "\"light_direction\": "
		    << dump_compact(arr3(s.light_direction.x, s.light_direction.y,
		                         s.light_direction.z))
		    << ", \"light_color\": "
		    << dump_compact(
		               arr3(s.light_color.x, s.light_color.y, s.light_color.z))
		    << ", \"light_intensity\": " << tidy(s.light_intensity)
		    << ", \"ambient\": "
		    << dump_compact(arr3(s.ambient.x, s.ambient.y, s.ambient.z))
		    // Additive within v3 (like the 2 -> 3 ids): an old document
		    // without "exposure" loads with the default below.
		    << ", \"exposure\": " << tidy(s.exposure) << "},\n";
	}
	out << "  \"entities\": [\n";
	bool first = true;
	for (const Entity root : scene.roots()) {
		if (!first) {
			out << ",\n";
		}
		first = false;
		write_entity(out, scene.registry(), root, codecs, "    ");
	}
	out << "\n  ]\n}";
	return out.str();
}

bool scene_from_json(Scene& scene, const std::string& text, std::string* error)
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
		return fail(std::string{"JSON parse error: "} + ex.what());
	}
	if (!root.is_object()) {
		return fail("scene document must be a JSON object");
	}

	const int version = root.value("version", 0);
	if (version <= 0) {
		return fail("scene document has no version");
	}
	if (version > kSceneFormatVersion) {
		return fail("unsupported scene version: " + std::to_string(version));
	}
	if (!migrate_scene_json(root, version, kSceneFormatVersion)) {
		return fail("cannot migrate scene from version " +
		            std::to_string(version));
	}

	// A scene that carries a settings block replaces the renderer's globals;
	// one that does not leaves them alone, which is what a pre-v3 document
	// relies on.
	SceneSettings settings;
	bool has_settings = false;
	if (root.contains("settings") && root.at("settings").is_object()) {
		const auto& s = root.at("settings");
		const auto read3 = [&s](const char* key, glm::vec3& out) {
			if (!s.contains(key) || !s.at(key).is_array() ||
			    s.at(key).size() != 3) {
				return;
			}
			out = {s.at(key)[0].get<float>(), s.at(key)[1].get<float>(),
			       s.at(key)[2].get<float>()};
		};
		read3("light_direction", settings.light_direction);
		read3("light_color", settings.light_color);
		read3("ambient", settings.ambient);
		if (s.contains("light_intensity") && s.at("light_intensity").is_number()) {
			settings.light_intensity = s.at("light_intensity").get<float>();
		}
		// Absent in documents written before exposure existed: keep the
		// default (1.0, neutral), so old v3 scenes load unchanged.
		if (s.contains("exposure") && s.at("exposure").is_number()) {
			settings.exposure = s.at("exposure").get<float>();
		}
		has_settings = true;
	}

	// Build into a temporary so a failure cannot corrupt a live scene.
	Scene temp{root.value("name", scene.name())};
	try {
		if (root.contains("entities")) {
			const auto& entities = root.at("entities");
			if (!entities.is_array()) {
				return fail("'entities' must be an array");
			}
			const ComponentCodecs& codecs = default_component_codecs();
			std::vector<std::string> warnings;
			for (const auto& entity_json : entities) {
				if (!entity_json.is_object()) {
					return fail("entity must be an object");
				}
				entity_from_json(temp, entity_json, codecs, &warnings);
			}
		}
	} catch (const std::exception& ex) {
		return fail(std::string{"scene parse error: "} + ex.what());
	}

	scene = std::move(temp);
	if (has_settings) {
		scene.set_settings(settings);
	}
	// Rebuild derived identity state on the committed scene: GUIDs for any
	// entity that predates identity, and every EntityRef's handle.
	scene.resolve_refs();
	return true;
}

bool save_scene(const Scene& scene, const std::string& path, std::string* error)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) {
			*error = "cannot open for writing: " + path;
		}
		return false;
	}
	out << to_json_string(scene) << '\n';
	if (!out) {
		if (error) {
			*error = "write failed: " + path;
		}
		return false;
	}
	return true;
}

bool load_scene(Scene& scene, const std::string& path, std::string* error)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		if (error) {
			*error = "cannot open for reading: " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	return scene_from_json(scene, buffer.str(), error);
}

} // namespace lotus
