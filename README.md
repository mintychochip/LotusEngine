# LotusEngine

A small C++ game engine: ECS scenes, a Vulkan renderer with Hi-Z occlusion, Jolt
physics, transform animation, region streaming, Lua scripts, and an in-engine
editor.

## Quick start

```bash
git submodule update --init --recursive
cmake -B build -G Ninja
cmake --build build -j"$(nproc)"
cd build && ./bin/lotus
```

Working directory should be `build/` so `scenes/`, `assets/`, and `prefabs/`
resolve correctly.

Rendering uses Vulkan 1.3 directly (dynamic rendering and synchronization2;
nothing links libvulkan, entry points load through SDL), so a Vulkan 1.3 device
is required; lavapipe (software) is enough for the tests and headless runs.

### Sample hub (`scenes/main.json`)

- **WASD** — move the `player` character (Shift to sprint).
- **F1** — editor (viewport pick uses physics when possible).
- **F2** — console (`help`, `scene.switch`, `scene.load_async`, …).
- **Ctrl+Q** — quit.
- **F2** `game.save` / `game.load` — player pose + active scene name (`saves/game.json`).

Movement is **camera-relative** on the XZ plane (WASD strafe relative to the view).

The gold **bob_prop** sphere plays the `sample_bob` animation clip from
`assets/`. Walk east (~40 m) toward the sign; when the camera enters another
region from `scenes/regions.json`, additive scenes load (one region per frame by
default).

### Tests

```bash
cd build && ./bin/lotus_tests
```

GPU occlusion tests: `./bin/lotus_tests --gtest_filter=OcclusionGpuTest.*`

The GPU tests need a Vulkan 1.3 device (lavapipe, from the `mesa-vulkan-drivers`
package, is enough). On a machine without one they skip and say why, while a
device that exists but breaks the renderer still fails them. `ctest` runs the
suite a second time with the drivers hidden to keep that true, and
`LOTUS_REQUIRE_VULKAN=1` turns a missing device into a failure (CI sets it).

Regenerate shaders (requires `glslangValidator`):

```bash
./shaders/build-shaders.sh
```

### Rendering geometry

Meshes resolved from a `MeshSource` are uploaded to the GPU once, in local
space, and every entity using one is a single instance record (model matrix and
material) of one indexed, instanced draw per albedo texture and mesh; the shadow
pass replays the same draws. Sprites and hand-built meshes with an empty
`Mesh::source` are written as world-space vertices every frame instead, so use
an empty source for geometry that changes at runtime. Cached meshes that stop
being drawn are released after `GpuRenderer::kMeshIdleFrames` frames.

Within the lit pass, geometry is rasterised into the lower half of the depth
range (`[0, 0.5]`, through the viewport) and the procedural sky is drawn last,
at depth 1.0, over the pixels nothing covered, so its fragment shader never runs
under a surface. Depth is clamped, not clipped, so geometry past the far plane
lands on the top of its range; the halved range keeps that apart from an
untouched pixel. Anything else added to those passes with a depth test has to
use the same range (see `GpuRenderer::draw` and `set_pass_viewport`).

### Render benchmark

`lotus_render_bench` renders a fixed, seeded synthetic scene (~950 entities,
shadows, SSAO, FXAA, sky, fog, Hi-Z occlusion) through the offscreen pipeline
one synchronous frame at a time and reports per-phase times (begin, build,
record, submit, GPU wait) plus a fidelity check of the final frame against
`bench/golden/render_bench.png`. `./autoresearch.sh` builds it and runs it on
the system's Vulkan device, on lavapipe and, when the machine has one, on an
integrated GPU (which can show costs a discrete card hides, such as readback
from write-combined memory); the metrics are documented in its header.

```bash
./autoresearch.sh
```

After an intentional visual change, regenerate the golden (lavapipe only; the
command is in the script header).

### Environment

| Variable | Effect |
|----------|--------|
| `LOTUS_NO_UI=1` | Hide editor UI |
| `LOTUS_MAX_FRAMES=N` | Exit after N frames (automation) |
| `LOTUS_CONSOLE="cmd; …"` | Run console commands at startup |
| `LOTUS_PRESENT_MODE=mailbox\|immediate` | Vulkan backend: swapchain present mode (default FIFO, also used when the surface lacks the requested mode) |
| `LOTUS_VK_VALIDATION=1\|sync` | Vulkan backend: Khronos validation layer when installed (`sync` adds synchronization validation); messages are printed and counted, and the Vulkan tests assert zero |

## Docs

Design specs live under `docs/superpowers/specs/`.

## RPG Game

A full 3D RPG built on the engine: real-time combat, NPCs with dialogue,
inventory, quests, loot, and a demo scene.

```bash
cd build && ./bin/lotus_rpg
```

### Play Mode

- **Play button** in the editor hierarchy panel (or press the button in the
  top-left) to enter play mode.
- **WASD** — move (camera-relative).
- **E** — interact with NPCs, chests, and items.
- **Space** — attack nearest enemy.
- **I** — toggle inventory.
- **Q** — toggle quest log.
- **C** — toggle character stats.
- **Quick slots** — click a consumable in the bar (e.g. Health Potion) to drink
  it (30 HP heal).
- **F1** — return to editor mode.

### Demo Scene (`scenes/rpg_demo.json`)

`scenes/rpg_demo.json` is generated on first run (not committed); fresh clones
create it automatically.

- **Player** — visible green cylinder avatar; starts with iron sword, potions,
  and 100 gold.
- **NPCs** — elder (quest giver), merchant, guard, villager.
- **Enemies** — 3 goblins and 1 orc with AI chase/attack behavior.
- **Quests** — "Goblin Threat" is completable: kill 3 goblins for XP, gold, and
  the Goblin Slayer Blade.
- **Loot** — chests with gold and potions, quest item (ancient amulet).

### RPG Components

All RPG components are serializable and editable in the editor:
`Stats`, `Health`, `Mana`, `Inventory`, `Combat`, `NPC`, `Loot`,
`Interactable`, `AI`, `Dialogue`, `Quest`.

### Mesh Primitives

New builtin meshes: `builtin:cylinder`, `builtin:cone`, `builtin:plane`,
`builtin:pyramid`, `builtin:torus`, `builtin:terrain` (50 m extent, 64×64 grid,
8 m peaks; collidable heightfield) (in addition to `builtin:cube` and
`builtin:sphere`).
