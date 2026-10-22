-- Scripted insect flight and wing flap.
-- One shared script drives every insect root; state is keyed by entity, and
-- wings are found by the stable "<root>_wing_<side><index>" names.

local state = {}

local function wing_list(name)
	local wings = {}
	for index = 0, 1 do
		local right = lotus.find(name .. "_wing_r" .. index)
		if right then
			wings[#wings + 1] = { entity = right, sign = 1.0, phase = index * 0.9 }
		end
		local left = lotus.find(name .. "_wing_l" .. index)
		if left then
			wings[#wings + 1] = { entity = left, sign = -1.0, phase = index * 0.9 }
		end
	end
	return wings
end

local function kind_of(name)
	if string.find(name, "dragonfly") then
		return "dragonfly"
	elseif string.find(name, "bee") then
		return "bee"
	end
	return "butterfly"
end

local script = {}

function script.on_update(entity, dt)
	local st = state[entity]
	if not st then
		local name = lotus.get_name(entity)
		local x, y, z = lotus.get_position(entity)
		st = {
			name = name,
			kind = kind_of(name),
			base = { x, y, z },
			t = 0.0,
			previous = { x, y, z },
			yaw = 0.0,
			wings = wing_list(name),
		}
		state[entity] = st
	end

	st.t = st.t + dt
	local t = st.t
	local bx, by, bz = st.base[1], st.base[2], st.base[3]
	local phase = (entity * 0.61803398875) % 6.28318530718
	local px, py, pz

	if st.kind == "dragonfly" then
		local speed = 1.55
		px = bx + 0.75 * math.sin(t * speed + phase)
		pz = bz + 0.55 * math.cos(t * speed * 0.83 + phase * 1.3)
		py = by + 0.18 * math.sin(t * 2.4 + phase)
	elseif st.kind == "bee" then
		local speed = 2.4
		px = bx + 0.28 * math.sin(t * speed + phase)
		pz = bz + 0.28 * math.sin(t * speed * 1.9 + phase * 2.0)
		py = by + 0.07 * math.sin(t * 4.1 + phase)
	else
		local speed = 0.85
		px = bx + 0.55 * math.sin(t * speed + phase)
		pz = bz + 0.45 * math.cos(t * speed * 0.72 + phase * 1.7)
		py = by + 0.26 * math.sin(t * 1.7 + phase * 0.8)
	end

	local dx = px - st.previous[1]
	local dz = pz - st.previous[3]
	if dx * dx + dz * dz > 0.0000001 then
		st.yaw = math.atan(dx, dz)
	end
	st.previous = { px, py, pz }

	lotus.set_position(entity, px, py, pz)
	lotus.set_rotation(entity, 0.0, st.yaw, 0.0)

	local frequency
	local amplitude
	local dihedral
	if st.kind == "dragonfly" then
		frequency, amplitude, dihedral = 13.0, 0.62, 0.08
	elseif st.kind == "bee" then
		frequency, amplitude, dihedral = 15.0, 0.72, 0.20
	else
		frequency, amplitude, dihedral = 7.5, 0.88, 0.30
	end
	local angle = t * frequency * 6.28318530718
	for _, wing in ipairs(st.wings) do
		lotus.set_rotation(
			wing.entity,
			0.0,
			0.0,
			wing.sign * (dihedral + amplitude * math.sin(angle + wing.phase))
		)
	end
end

return script
