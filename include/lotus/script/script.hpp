#pragma once

#include <string>

namespace lotus {

/// Attaches a Lua script file to an entity.
///
/// The script must return a table that may define:
///   function script.on_start(entity)
///   function script.on_update(entity, dt)
///
/// Inside a script the `lotus` table exposes:
///   lotus.find(name)                -> entity | nil
///   lotus.get_position(e)           -> x, y, z
///   lotus.set_position(e, x, y[, z])
///   lotus.rotate(e, radians)        -- adds to rotation.z
///   lotus.set_color(e, r, g, b)
///   lotus.get_name(e)               -> string
struct Script {
	std::string path;
};

} // namespace lotus
