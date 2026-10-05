#include "lotus/scene/serialization.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

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
}

void from_json(const nlohmann::json& j, Script& s)
{
	if (j.is_object() && j.contains("path") && j.at("path").is_string()) {
		s.path = j.at("path").get<std::string>();
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

const ComponentCodecs& default_component_codecs()
{
	static const ComponentCodecs codecs = [] {
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
		return c;
	}();
	return codecs;
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
		if (!components.is_object()) {
			throw std::runtime_error("'components' must be an object");
		}
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

std::string to_json_string(const Scene& scene)
{
	const ComponentCodecs& codecs = default_component_codecs();
	std::ostringstream out;
	out << "{\n";
	out << "  \"version\": " << kSceneFormatVersion << ",\n";
	out << "  \"name\": " << nlohmann::json(scene.name()).dump() << ",\n";
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
	if (version <= 0 || version > kSceneFormatVersion) {
		return fail("unsupported scene version: " + std::to_string(version));
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
