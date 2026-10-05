-- Example script: rotates the entity it is attached to.
-- Edit this file while the engine is running and it reloads live.

local script = {}

function script.on_start(entity)
    local x, y, z = lotus.get_position(entity)
    lotus.set_position(entity, x, y, z)
end

function script.on_update(entity, dt)
    lotus.rotate(entity, dt * 2.0)
end

return script
