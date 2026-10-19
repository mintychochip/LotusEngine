# LotusEngine

A small C++ game engine: ECS scenes, SDL GPU renderer with Hi-Z occlusion, Jolt
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

Regenerate shaders (requires `glslangValidator`):

```bash
./shaders/build-shaders.sh
```

### Render benchmark

`lotus_render_bench` renders a fixed, seeded synthetic scene (~950 entities,
shadows, SSAO, FXAA, sky, fog, Hi-Z occlusion) through the offscreen pipeline
one synchronous frame at a time and reports per-phase times (begin, build,
record, submit, GPU wait) plus a fidelity check of the final frame against
`bench/golden/render_bench.png`. `./autoresearch.sh` builds it and runs it on
the system's Vulkan device and on lavapipe; the metrics are documented in its
header.

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
