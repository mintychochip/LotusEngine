#include "lotus/script/script_engine.hpp"

// Lua's headers have no extern "C" guards of their own.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "lotus/scene/scene.hpp"
#include "lotus/script/script.hpp"

namespace lotus {
namespace {

constexpr const char* kSceneRegistryKey = "lotus.scene";

void store_current_scene(lua_State* L, Scene* scene)
{
	lua_pushlightuserdata(L, scene);
	lua_setfield(L, LUA_REGISTRYINDEX, kSceneRegistryKey);
}

Scene* current_scene(lua_State* L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, kSceneRegistryKey);
	Scene* scene = static_cast<Scene*>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	return scene;
}

Entity entity_arg(lua_State* L, int index)
{
	return static_cast<Entity>(lua_tointeger(L, index));
}

Transform* transform_arg(lua_State* L)
{
	Scene* scene = current_scene(L);
	if (!scene) {
		return nullptr;
	}
	const Entity entity = entity_arg(L, 1);
	if (!scene->registry().valid(entity)) {
		return nullptr;
	}
	return scene->registry().try_get<Transform>(entity);
}

int l_find(lua_State* L)
{
	Scene* scene = current_scene(L);
	const char* name = luaL_checkstring(L, 1);
	if (!scene) {
		lua_pushnil(L);
		return 1;
	}
	const Entity entity = scene->find(name);
	if (entity == NullEntity) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, static_cast<lua_Integer>(entt::to_integral(entity)));
	return 1;
}

int l_get_position(lua_State* L)
{
	const Transform* transform = transform_arg(L);
	if (!transform) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, transform->position.x);
	lua_pushnumber(L, transform->position.y);
	lua_pushnumber(L, transform->position.z);
	return 3;
}

int l_set_position(lua_State* L)
{
	Transform* transform = transform_arg(L);
	if (!transform) {
		return 0;
	}
	transform->position.x = static_cast<float>(luaL_checknumber(L, 2));
	transform->position.y = static_cast<float>(luaL_checknumber(L, 3));
	if (!lua_isnoneornil(L, 4)) {
		transform->position.z = static_cast<float>(luaL_checknumber(L, 4));
	}
	return 0;
}

int l_rotate(lua_State* L)
{
	Transform* transform = transform_arg(L);
	if (!transform) {
		return 0;
	}
	transform->rotation.z += static_cast<float>(luaL_checknumber(L, 2));
	return 0;
}

int l_set_color(lua_State* L)
{
	Scene* scene = current_scene(L);
	if (!scene) {
		return 0;
	}
	const Entity entity = entity_arg(L, 1);
	auto* sprite = scene->registry().valid(entity)
	                     ? scene->registry().try_get<Sprite>(entity)
	                     : nullptr;
	if (!sprite) {
		return 0;
	}
	sprite->color.r = static_cast<float>(luaL_checknumber(L, 2));
	sprite->color.g = static_cast<float>(luaL_checknumber(L, 3));
	sprite->color.b = static_cast<float>(luaL_checknumber(L, 4));
	return 0;
}

int l_get_name(lua_State* L)
{
	Scene* scene = current_scene(L);
	if (!scene) {
		lua_pushstring(L, "");
		return 1;
	}
	const Entity entity = entity_arg(L, 1);
	if (!scene->registry().valid(entity)) {
		lua_pushstring(L, "");
		return 1;
	}
	const auto* name = scene->registry().try_get<Name>(entity);
	lua_pushstring(L, name ? name->value.c_str() : "");
	return 1;
}

void register_api(lua_State* L)
{
	lua_newtable(L);
	const struct {
		const char* name;
		lua_CFunction fn;
	} functions[] = {
	        {"find", l_find},
	        {"get_position", l_get_position},
	        {"set_position", l_set_position},
	        {"rotate", l_rotate},
	        {"set_color", l_set_color},
	        {"get_name", l_get_name},
	};
	for (const auto& entry : functions) {
		lua_pushcfunction(L, entry.fn);
		lua_setfield(L, -2, entry.name);
	}
	lua_setglobal(L, "lotus");
}

} // namespace

ScriptEngine::ScriptEngine()
{
	state_ = luaL_newstate();
	if (state_) {
		luaL_openlibs(state_);
		register_api(state_);
	}
}

ScriptEngine::~ScriptEngine()
{
	if (state_) {
		for (const auto& [path, script] : scripts_) {
			(void)path;
			luaL_unref(state_, LUA_REGISTRYINDEX, script.ref);
		}
		lua_close(state_);
		state_ = nullptr;
	}
}

std::string ScriptEngine::resolve(const std::string& path) const
{
	if (root_.empty() || path.empty()) {
		return path;
	}
	const std::filesystem::path candidate{path};
	if (candidate.is_absolute()) {
		return path;
	}
	return (std::filesystem::path{root_} / candidate).string();
}

bool ScriptEngine::is_loaded(const std::string& path) const
{
	return scripts_.find(path) != scripts_.end();
}

bool ScriptEngine::load(const std::string& path, std::string* error)
{
	if (!state_) {
		if (error) {
			*error = "script engine has no Lua state";
		}
		return false;
	}
	const std::string full = resolve(path);
	std::ifstream in(full, std::ios::binary);
	if (!in) {
		const std::string message = "cannot open script: " + full;
		last_error_               = message;
		if (error) {
			*error = message;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	const std::string source = buffer.str();

	// Compile and run into a fresh value; only commit on success so a broken
	// edit cannot destroy a working script.
	if (luaL_loadbuffer(state_, source.data(), source.size(), full.c_str()) !=
	    LUA_OK) {
		const std::string message =
		        std::string{"script compile error: "} + lua_tostring(state_, -1);
		lua_pop(state_, 1);
		last_error_ = message;
		if (error) {
			*error = message;
		}
		return false;
	}
	if (lua_pcall(state_, 0, 1, 0) != LUA_OK) {
		const std::string message =
		        std::string{"script runtime error: "} + lua_tostring(state_, -1);
		lua_pop(state_, 1);
		last_error_ = message;
		if (error) {
			*error = message;
		}
		return false;
	}
	if (!lua_istable(state_, -1)) {
		lua_pop(state_, 1);
		const std::string message = "script must return a table: " + full;
		last_error_               = message;
		if (error) {
			*error = message;
		}
		return false;
	}

	lua_pushvalue(state_, -1);
	const int ref = luaL_ref(state_, LUA_REGISTRYINDEX);
	lua_pop(state_, 1);

	const auto existing = scripts_.find(path);
	if (existing != scripts_.end()) {
		luaL_unref(state_, LUA_REGISTRYINDEX, existing->second.ref);
		existing->second.ref = ref;
	} else {
		scripts_.emplace(path, ScriptRef{ref});
	}
	watcher_.watch(path, full);
	last_error_.clear();
	return true;
}

void ScriptEngine::set_current_scene(Scene* scene)
{
	store_current_scene(state_, scene);
}

void ScriptEngine::call_hook(Scene& scene, Entity entity, const char* hook,
                             bool with_dt, float dt)
{
	const auto* script = scene.registry().try_get<Script>(entity);
	if (!script) {
		return;
	}
	const auto it = scripts_.find(script->path);
	if (it == scripts_.end()) {
		return;
	}
	lua_rawgeti(state_, LUA_REGISTRYINDEX, it->second.ref);
	if (!lua_istable(state_, -1)) {
		lua_pop(state_, 1);
		return;
	}
	lua_getfield(state_, -1, hook);
	if (!lua_isfunction(state_, -1)) {
		lua_pop(state_, 2);
		return;
	}
	lua_pushinteger(state_, static_cast<lua_Integer>(entt::to_integral(entity)));
	int arguments = 1;
	if (with_dt) {
		lua_pushnumber(state_, dt);
		arguments = 2;
	}
	if (lua_pcall(state_, arguments, 0, 0) != LUA_OK) {
		const char* message = lua_tostring(state_, -1);
		last_error_         = std::string{hook} + " error: " +
		              (message ? message : "unknown");
		lua_pop(state_, 1);
	}
	lua_pop(state_, 1); // the script table
}

void ScriptEngine::start_entities(Scene& scene, const std::string& path)
{
	auto view = scene.registry().view<Script>();
	for (const Entity entity : view) {
		if (view.get<Script>(entity).path == path) {
			call_hook(scene, entity, "on_start", false, 0.0f);
		}
	}
}

bool ScriptEngine::sync(Scene& scene, std::string* error)
{
	set_current_scene(&scene);
	std::unordered_set<std::string> paths;
	for (const Entity entity : scene.registry().view<Script>()) {
		paths.insert(scene.registry().get<Script>(entity).path);
	}

	bool ok = true;
	for (const std::string& path : paths) {
		if (is_loaded(path)) {
			continue;
		}
		std::string failure;
		if (!load(path, &failure)) {
			ok = false;
			if (error) {
				*error = failure;
			}
			continue;
		}
	}
	// Start entities whose scripts are available.
	for (const std::string& path : paths) {
		if (is_loaded(path)) {
			start_entities(scene, path);
		}
	}
	return ok;
}

void ScriptEngine::update(Scene& scene, float dt)
{
	if (!state_) {
		return;
	}
	set_current_scene(&scene);
	for (const Entity entity : scene.registry().view<Script>()) {
		call_hook(scene, entity, "on_update", true, dt);
	}
}

void ScriptEngine::poll(Scene& scene)
{
	if (!state_) {
		return;
	}
	set_current_scene(&scene);
	for (const std::string& path : watcher_.poll()) {
		std::string failure;
		if (load(path, &failure)) {
			start_entities(scene, path);
			if (log_) {
				log_("hot-swapped script: " + path);
			}
		} else {
			last_error_ = "script reload rejected: " + failure;
			if (log_) {
				log_(last_error_);
			}
		}
	}
}

} // namespace lotus
